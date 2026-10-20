// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// input scaffolding: dummy core devices so clients see a working
// keyboard and pointer from the first start. the input wave of this
// phase feeds them from X12 seat events instead of leaving them
// silent. device creation follows the Xvfb pattern, trimmed to a
// three button pointer

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <X11/X.h>
#include "mi.h"
#include <X11/Xproto.h>
#include "scrnintstr.h"
#include "inputstr.h"
#include "mipointer.h"
#include "xkbsrv.h"
#include "xserver-properties.h"
#include "exevents.h"
#include "extinit.h"
#include "inpututils.h"

#include "x12-client.h"

static DeviceIntPtr x12_pointer_dev;
static DeviceIntPtr x12_keyboard_dev;

void
x12_bridge_input_motion(int32_t x, int32_t y)
{
    if (!x12_pointer_dev) {
        return;
    }
    // the rootful surface covers the whole screen, surface local
    // coordinates are screen coordinates already
    ValuatorMask mask;
    valuator_mask_zero(&mask);
    valuator_mask_set(&mask, 0, x);
    valuator_mask_set(&mask, 1, y);
    QueuePointerEvents(x12_pointer_dev, MotionNotify, 0,
                       POINTER_ABSOLUTE | POINTER_SCREEN, &mask);
}

void
x12_bridge_input_button(uint32_t button, int pressed)
{
    if (!x12_pointer_dev) {
        return;
    }
    // the x12 numbering is 1 left, 2 right, 3 middle, dix wants the
    // classic 2 middle, 3 right
    int index = button == 2 ? 3 : button == 3 ? 2 : (int)button;
    ValuatorMask mask;
    valuator_mask_zero(&mask);
    QueuePointerEvents(x12_pointer_dev,
                       pressed ? ButtonPress : ButtonRelease, index, 0,
                       &mask);
}

void
x12_bridge_input_axis(int32_t kind, int32_t value)
{
    if (!x12_pointer_dev) {
        return;
    }
    // wheel clicks ride the legacy scroll buttons, positive vertical
    // is down, positive horizontal is right
    int notches = value < 0 ? -value : value;
    int button = kind == 1 ? (value > 0 ? 7 : 6)
                           : (value > 0 ? 5 : 4);
    notches /= 256;
    ValuatorMask mask;
    valuator_mask_zero(&mask);
    for (int i = 0; i < notches; i++) {
        QueuePointerEvents(x12_pointer_dev, ButtonPress, button, 0,
                           &mask);
        QueuePointerEvents(x12_pointer_dev, ButtonRelease, button, 0,
                           &mask);
    }
}

void
x12_bridge_input_key(uint32_t code, int pressed)
{
    if (!x12_keyboard_dev) {
        return;
    }
    // evdev code to dix keycode, the xfree86 convention shift
    QueueKeyboardEvents(x12_keyboard_dev,
                        pressed ? KeyPress : KeyRelease, (int)code + 8);
}

void
ProcessInputEvents(void)
{
    mieqProcessInputEvents();
}

void
DDXRingBell(int volume, int pitch, int duration)
{
    (void)volume;
    (void)pitch;
    (void)duration;
}

static int
x12KeybdProc(DeviceIntPtr pDevice, int onoff)
{
    DevicePtr pDev = (DevicePtr)pDevice;

    switch (onoff) {
    case DEVICE_INIT:
        InitKeyboardDeviceStruct(pDevice, NULL, NULL, NULL);
        break;
    case DEVICE_ON:
        pDev->on = TRUE;
        break;
    case DEVICE_OFF:
        pDev->on = FALSE;
        break;
    case DEVICE_CLOSE:
        break;
    }
    return Success;
}

#define X12_NBUTTONS 3
#define X12_NAXES 2

static int
x12MouseProc(DeviceIntPtr pDevice, int onoff)
{
    BYTE map[X12_NBUTTONS + 1];
    DevicePtr pDev = (DevicePtr)pDevice;
    Atom btn_labels[X12_NBUTTONS] = { 0 };
    Atom axes_labels[X12_NAXES] = { 0 };

    switch (onoff) {
    case DEVICE_INIT:
        for (int i = 1; i <= X12_NBUTTONS; ++i) {
            map[i] = i;
        }
        btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
        btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
        btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);

        InitPointerDeviceStruct(pDev, map, X12_NBUTTONS, btn_labels,
                                (PtrCtrlProcPtr)NoopDDA,
                                GetMotionHistorySize(), X12_NAXES,
                                axes_labels);
        break;
    case DEVICE_ON:
        pDev->on = TRUE;
        break;
    case DEVICE_OFF:
        pDev->on = FALSE;
        break;
    case DEVICE_CLOSE:
        break;
    }
    return Success;
}

void
InitInput(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    DeviceIntPtr p, k;
    Atom xiclass;

    p = AddInputDevice(serverClient, x12MouseProc, TRUE);
    k = AddInputDevice(serverClient, x12KeybdProc, TRUE);
    x12_pointer_dev = p;
    x12_keyboard_dev = k;
    xiclass = MakeAtom(XI_MOUSE, sizeof(XI_MOUSE) - 1, TRUE);
    AssignTypeAndName(p, xiclass, "X12 pointer");
    xiclass = MakeAtom(XI_KEYBOARD, sizeof(XI_KEYBOARD) - 1, TRUE);
    AssignTypeAndName(k, xiclass, "X12 keyboard");
    (void)mieqInit();
}

void
CloseInput(void)
{
    mieqFini();
}
