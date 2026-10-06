/* vdd_joy.c -- see vdd_joy.h.  Gameport ports 0x200-0x207 on the VDD bus; the
 * 558 one-shot model, timed by the host-injected microsecond clock.  No Windows
 * calls, only Windows types. */
#include "vdd_joy.h"

/* IN: buttons in bits 4-7 (0 = pressed), one-shot state in bits 0-3.
   An axis the adapter does not wire (JoystickType) or with nothing plugged in
   reads STUCK HIGH once triggered -- an open resistive input never reaches the
   558's threshold, so the pulse never ends. That is what a game's detection
   loop times out on to decide "no joystick", and it is also what an UNCLAIMED
   port looked like (0xFF), so a machine with the type set to None is
   indistinguishable from one without the card. */
#define JOYSTICK_FIRST_PORT         0x200
#define JOYSTICK_LAST_PORT          0x207  /* a real card decodes the whole block       */
#define JOYSTICK_NO_BUTTONS_PRESSED 0xF0   /* bits 4-7 high: active-low buttons up      */
#define JOYSTICK_NO_CARD            0xFF   /* what an unclaimed port reads              */
#define JOYSTICK_BUTTON_SHIFT       4      /* buttons live in bits 4-7                  */
#define JOYSTICK_NO_ELAPSED_TIME    0      /* no clock: pulses never end                */

static VOID VddJoystickPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PJOYSTICK_STATE state = (PJOYSTICK_STATE)context; (VOID)port; (VOID)width;
    BYTE result = JOYSTICK_NO_BUTTONS_PRESSED;   /* no buttons pressed          */
    INT axisIndex, wiredAxes = VddJoystickAxes(state);
    state->PortReads++;
    if (state->Type == JOYSTICK_TYPE_NONE) { *value = JOYSTICK_NO_CARD; return; }
    if (VddJoystickIsLive(state)) {
        BYTE buttonMask = (BYTE)((1u << VddJoystickButtonsWired(state)) - 1u);
        result = (BYTE)((BYTE)(~(state->Buttons & buttonMask)) << JOYSTICK_BUTTON_SHIFT);
    }
    if (state->HasFired) {
        UINT64 elapsed = state->NowMicroseconds
                    ? state->NowMicroseconds(state->ClockContext) - state->TriggerMicroseconds
                    : JOYSTICK_NO_ELAPSED_TIME;  /* no clock: pulses never end  */
        for (axisIndex = 0; axisIndex < JOYSTICK_AXES; ++axisIndex) {
            INT isStuck = (axisIndex >= wiredAxes) || !state->IsPresent || !state->NowMicroseconds;
            if (isStuck || elapsed < VddJoystickAxisMicroseconds(state->Axis[axisIndex])) result |= (BYTE)(1u << axisIndex);
        }
    }
    *value = result;
}

/* OUT (any value, any port in the range): fire the one-shots. */
static VOID VddJoystickPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PJOYSTICK_STATE state = (PJOYSTICK_STATE)context; (VOID)port; (VOID)width; (VOID)value;
    state->PortWrites++;
    if (state->Type == JOYSTICK_TYPE_NONE) return;   /* no card, nothing to fire   */
    state->HasFired = TRUE;
    state->TriggerMicroseconds = state->NowMicroseconds ? state->NowMicroseconds(state->ClockContext) : 0;
}

VOID VddJoystickReset(PVOID context)
{
    PJOYSTICK_STATE state = (PJOYSTICK_STATE)context;
    state->HasFired = FALSE; state->TriggerMicroseconds = 0; /* keep Type + the host-fed sample */
}

INT VddJoystickInitialize(PVDD_BUS bus, PVOID context)
{
    PJOYSTICK_STATE state = (PJOYSTICK_STATE)context;
    state->Bus = bus;
    /* A real gameport card decodes the whole 0x200-0x207 block. */
    return VddClaimPorts(bus, JOYSTICK_FIRST_PORT, JOYSTICK_LAST_PORT, VddJoystickPortIn, VddJoystickPortOut, state);
}
