// bo1mp - Call of Duty: Black Ops (multiplayer), recompiled with ReXGlue
//
// The application is the same as the campaign one (../src/bo1_app.h); only the multiplayer
// parameters are set here.

#include "generated/default_mp/bo1mp_init.h"

#define BO1_APP_NAME "bo1mp"
#define BO1_XEX_NAME "default_mp.xex"
#define BO1_WINDOW_TITLE "Call of Duty: Black Ops - Multiplayer (recompiled)"
#include "bo1_app.h"

REX_DEFINE_APP(bo1mp, Bo1App::Create)
