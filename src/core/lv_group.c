/**
 * @file lv_group.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include <stddef.h>

#include "lv_group.h"
#include "../misc/lv_gc.h"
#include "../core/lv_obj.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void focus_next_core(lv_group_t * group, void * (*begin)(const lv_ll_t *),
                            void * (*move)(const lv_ll_t *, const void *));
static void lv_group_refocus(lv_group_t * g);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Init. the group module
 */
void _lv_group_init(void)
{
    _lv_ll_init(&LV_GC_ROOT(_lv_group_ll), sizeof(lv_group_t));
}

/**
 * Create a new object group
 * @return pointer to the new object group
 */
lv_group_t * lv_group_create(void)
{
    lv_group_t * group = _lv_ll_ins_head(&LV_GC_ROOT(_lv_group_ll));
    LV_ASSERT_MALLOC(group);
    if(group == NULL) return NULL;
    _lv_ll_init(&group->obj_ll, sizeof(lv_obj_t *));

    group->obj_focus      = NULL;
    group->frozen         = 0;
    group->focus_cb       = NULL;
    group->click_focus    = 1;
    group->editing        = 0;
    group->refocus_policy = LV_GROUP_REFOCUS_POLICY_PREV;
    group->wrap           = 1;

#if LV_USE_USER_DATA
    group->user_data = NULL;
#endif

    return group;
}

/**
 * Delete a group object
 * @param group pointer to a group
 */
void lv_group_del(lv_group_t * group)
{
    /*Defocus the currently focused object*/
    if(group->obj_focus != NULL) {
        lv_event_send(*group->obj_focus, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_invalidate(*group->obj_focus);
    }

    /*Remove the objects from the group*/
    lv_obj_t ** obj;
    _LV_LL_READ(&group->obj_ll, obj) {
        if((*obj)->spec_attr) (*obj)->spec_attr->group_p = NULL;
    }

    _lv_ll_clear(&(group->obj_ll));
    _lv_ll_remove(&LV_GC_ROOT(_lv_group_ll), group);
    lv_mem_free(group);
}

/**
 * Add an object to a group
 * @param group pointer to a group
 * @param obj pointer to an object to add
 */
void lv_group_add_obj(lv_group_t * group, lv_obj_t * obj)
{
    if(group == NULL) return;

    LV_LOG_TRACE("begin");

    /*Do not add the object twice*/
    lv_obj_t ** obj_i;
    _LV_LL_READ(&group->obj_ll, obj_i) {
        if((*obj_i) == obj) {
            LV_LOG_INFO("the object is already added to this group");
            return;
        }
    }

    /*If the object is already in a group and focused then refocus it*/
    lv_group_t * group_cur = lv_obj_get_group(obj);
    if(group_cur) {
        if(obj->spec_attr->group_p && *(obj->spec_attr->group_p->obj_focus) == obj) {
            lv_group_refocus(group_cur);

            LV_LOG_INFO("changing object's group");
        }
    }

    if(obj->spec_attr == NULL) lv_obj_allocate_spec_attr(obj);
    obj->spec_attr->group_p = group;

    lv_obj_t ** next = _lv_ll_ins_tail(&group->obj_ll);
    LV_ASSERT_MALLOC(next);
    if(next == NULL) return;
    *next = obj;

    /* If the head and the tail is equal then there is only one object in the linked list.
     * In this case automatically activate it*/
    if(_lv_ll_get_head(&group->obj_ll) == next) {
        lv_group_refocus(group);
    }

    LV_LOG_TRACE("finished");
}

/**
 * Remove an object from its group
 * @param obj pointer to an object to remove
 */
void lv_group_remove_obj(lv_obj_t * obj)
{
    lv_group_t * g = lv_obj_get_group(obj);
    if(g == NULL) return;

    LV_LOG_TRACE("begin");

    /*Focus on the next object*/
    if(*g->obj_focus == obj) {
        if(g->frozen) g->frozen = 0;

        /*If this is the only object in the group then focus to nothing.*/
        if(_lv_ll_get_head(&g->obj_ll) == g->obj_focus && _lv_ll_get_tail(&g->obj_ll) == g->obj_focus) {
            lv_event_send(*g->obj_focus, LV_EVENT_DEFOCUSED, NULL);
        }
        /*If there more objects in the group then focus to the next/prev object*/
        else {
            lv_group_refocus(g);
        }
    }

    /* If the focuses object is still the same then it was the only object in the group but it will
     * be deleted. Set the `obj_focus` to NULL to get back to the initial state of the group with
     * zero objects*/
    if(*g->obj_focus == obj) {
        g->obj_focus = NULL;
    }

    /*Search the object and remove it from its group */
    lv_obj_t ** i;
    _LV_LL_READ(&g->obj_ll, i) {
        if(*i == obj) {
            _lv_ll_remove(&g->obj_ll, i);
            lv_mem_free(i);
            if(obj->spec_attr) obj->spec_attr->group_p = NULL;
            break;
        }
    }
    LV_LOG_TRACE("finished");
}

/**
 * Remove all objects from a group
 * @param group pointer to a group
 */
void lv_group_remove_all_objs(lv_group_t * group)
{
    /*Defocus the currently focused object*/
    if(group->obj_focus != NULL) {
        lv_event_send(*group->obj_focus, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_invalidate(*group->obj_focus);
        group->obj_focus = NULL;
    }

    /*Remove the objects from the group*/
    lv_obj_t ** obj;
    _LV_LL_READ(&group->obj_ll, obj) {
        if((*obj)->spec_attr) (*obj)->spec_attr->group_p = NULL;
    }

    _lv_ll_clear(&(group->obj_ll));
}

/**
 * Focus on an object (defocus the current)
 * @param obj pointer to an object to focus on
 */
void lv_group_focus_obj(lv_obj_t * obj)
{
    if(obj == NULL) return;
    lv_group_t * g = lv_obj_get_group(obj);
    if(g == NULL) return;

    if(g->frozen != 0) return;

    if(g->obj_focus != NULL && obj == *g->obj_focus) return;

    /*On defocus edit mode must be leaved*/
    lv_group_set_editing(g, false);

    lv_obj_t ** i;
    _LV_LL_READ(&g->obj_ll, i) {
        if(*i == obj) {
            if(g->obj_focus != NULL) {
                lv_res_t res = lv_event_send(*g->obj_focus, LV_EVENT_DEFOCUSED, NULL);
                if(res != LV_RES_OK) return;
                lv_obj_invalidate(*g->obj_focus);
            }

            g->obj_focus = i;

            if(g->obj_focus != NULL) {
                if(g->focus_cb) g->focus_cb(g);
                lv_res_t res = lv_event_send(*g->obj_focus, LV_EVENT_FOCUSED, NULL);
                if(res != LV_RES_OK) return;
                lv_obj_invalidate(*g->obj_focus);
            }
            break;
        }
    }
}

/**
 * Focus the next object in a group (defocus the current)
 * @param group pointer to a group
 */
void lv_group_focus_next(lv_group_t * group)
{
    focus_next_core(group, _lv_ll_get_head, _lv_ll_get_next);
}

/**
 * Focus the previous object in a group (defocus the current)
 * @param group pointer to a group
 */
void lv_group_focus_prev(lv_group_t * group)
{
    focus_next_core(group, _lv_ll_get_tail, _lv_ll_get_prev);
}

/**
 * Do not let to change the focus from the current object
 * @param group pointer to a group
 * @param en true: freeze, false: release freezing (normal mode)
 */
void lv_group_focus_freeze(lv_group_t * group, bool en)
{
    if(en == false)
        group->frozen = 0;
    else
        group->frozen = 1;
}

/**
 * Send a control character to the focuses object of a group
 * @param group pointer to a group
 * @param c a character (use LV_KEY_.. to navigate)
 * @return result of focused object in group.
 */
lv_res_t lv_group_send_data(lv_group_t * group, uint32_t c)
{
    lv_obj_t * act = lv_group_get_focused(group);
    if(act == NULL) return LV_RES_OK;

    lv_res_t res;

    res = lv_event_send(act, LV_EVENT_KEY, &c);
    if(res != LV_RES_OK) return res;

    return res;
}

/**
 * Set a function for a group which will be called when a new object is focused
 * @param group pointer to a group
 * @param focus_cb the call back function or NULL if unused
 */
void lv_group_set_focus_cb(lv_group_t * group, lv_group_focus_cb_t focus_cb)
{
    group->focus_cb = focus_cb;
}

/**
 * Manually set the current mode (edit or navigate).
 * @param group pointer to group
 * @param edit true: edit mode; false: navigate mode
 */
void lv_group_set_editing(lv_group_t * group, bool edit)
{
    if(group == NULL) return;
    uint8_t en_val = edit ? 1 : 0;

    if(en_val == group->editing) return; /*Do not set the same mode again*/

    group->editing     = en_val;
    lv_obj_t * focused = lv_group_get_focused(group);

    if(focused) {
        lv_res_t res = lv_event_send(*group->obj_focus, LV_EVENT_FOCUSED, NULL);
        if(res != LV_RES_OK) return;

        lv_obj_invalidate(focused);
    }
}

/**
 * Set the `click_focus` attribute. If enabled then the object will be focused then it is clicked.
 * @param group pointer to group
 * @param en true: enable `click_focus`
 */
void lv_group_set_click_focus(lv_group_t * group, bool en)
{
    group->click_focus = en ? 1 : 0;
}

void lv_group_set_refocus_policy(lv_group_t * group, lv_group_refocus_policy_t policy)
{
    group->refocus_policy = policy & 0x01;
}

/**
 * Set whether focus next/prev will allow wrapping from first->last or last->first.
 * @param group pointer to group
 * @param en true: enable `wrap`
 */
void lv_group_set_wrap(lv_group_t * group, bool en)
{
    group->wrap = en ? 1 : 0;
}

/**
 * Get the focused object or NULL if there isn't one
 * @param group pointer to a group
 * @return pointer to the focused object
 */
lv_obj_t * lv_group_get_focused(const lv_group_t * group)
{
    if(!group) return NULL;
    if(group->obj_focus == NULL) return NULL;

    return *group->obj_focus;
}

/**
 * Get the focus callback function of a group
 * @param group pointer to a group
 * @return the call back function or NULL if not set
 */
lv_group_focus_cb_t lv_group_get_focus_cb(const lv_group_t * group)
{
    if(!group) return NULL;
    return group->focus_cb;
}

/**
 * Get the current mode (edit or navigate).
 * @param group pointer to group
 * @return true: edit mode; false: navigate mode
 */
bool lv_group_get_editing(const lv_group_t * group)
{
    if(!group) return false;
    return group->editing ? true : false;
}

/**
 * Get the `click_focus` attribute.
 * @param group pointer to group
 * @return true: `click_focus` is enabled; false: disabled
 */
bool lv_group_get_click_focus(const lv_group_t * group)
{
    if(!group) return false;
    return group->click_focus ? true : false;
}

/**
 * Get whether focus next/prev will allow wrapping from first->last or last->first object.
 * @param group pointer to group
 * @param en true: wrapping enabled; false: wrapping disabled
 */
bool lv_group_get_wrap(lv_group_t * group)
{
    if(!group) return false;
    return group->wrap ? true : false;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_group_refocus(lv_group_t * g)
{
    /*Refocus must temporarily allow wrapping to work correctly*/
    uint8_t temp_wrap = g->wrap;
    g->wrap           = 1;

    if(g->refocus_policy == LV_GROUP_REFOCUS_POLICY_NEXT)
        lv_group_focus_next(g);
    else if(g->refocus_policy == LV_GROUP_REFOCUS_POLICY_PREV)
        lv_group_focus_prev(g);
    /*Restore wrap property*/
    g->wrap = temp_wrap;
}

static void focus_next_core(lv_group_t * group, void * (*begin)(const lv_ll_t *),
                            void * (*move)(const lv_ll_t *, const void *))
{
    if(group->frozen) return;

    lv_obj_t ** obj_next     = group->obj_focus;
    lv_obj_t ** obj_sentinel = NULL;
    bool can_move            = true;
    bool can_begin           = true;

    for(;;) {
        if(obj_next == NULL) {
            if(group->wrap || obj_sentinel == NULL) {
                if(!can_begin) return;
                obj_next  = begin(&group->obj_ll);
                can_move  = false;
                can_begin = false;
            }
            else {
                /*Currently focused object is the last/first in the group, keep it that way*/
                return;
            }
        }

        if(obj_sentinel == NULL) {
            obj_sentinel = obj_next;
            if(obj_sentinel == NULL) return; /*Group is empty*/
        }

        if(can_move) {
            obj_next = move(&group->obj_ll, obj_next);

            /*Give up if we walked the entire list and haven't found another visible object*/
            if(obj_next == obj_sentinel) return;
        }

        can_move = true;

        if(obj_next == NULL) continue;
        if(lv_obj_get_state(*obj_next) & LV_STATE_DISABLED) continue;

        /*Hidden objects don't receive focus*/
        if(lv_obj_has_flag(*obj_next, LV_OBJ_FLAG_HIDDEN) == false) break;
    }

    if(obj_next == group->obj_focus) return; /*There's only one visible object and it's already focused*/

    if(group->obj_focus) {
        lv_res_t res = lv_event_send(*group->obj_focus, LV_EVENT_DEFOCUSED, NULL);
        if(res != LV_RES_OK) return;
        lv_obj_invalidate(*group->obj_focus);
    }

    group->obj_focus = obj_next;

    lv_res_t res = lv_event_send(*group->obj_focus, LV_EVENT_FOCUSED, NULL);
    if(res != LV_RES_OK) return;

    lv_obj_invalidate(*group->obj_focus);

    if(group->focus_cb) group->focus_cb(group);
}
