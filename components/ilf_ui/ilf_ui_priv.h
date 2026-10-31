/* Внутреннее для ilf_ui.c и ilf_ui_setup.c */
#pragma once
#include "ilf_ui.h"

void ilf_ui_lock_show_pad(void);
void ilf_ui_lock_state(bool locked, ilf_ui_pin_cb cb, void *user);
