/* settings_ids.h -- control IDs for the Settings dialog.
 *
 * Included by BOTH res/ntvdmhost.rc (which windres compiles) and src/host/settings.h.
 * That is why it contains nothing but #defines: windres understands the preprocessor
 * and nothing else, so a stray declaration here breaks the resource build rather than
 * the C build, which is a confusing place to find out.
 */
#ifndef NTVDMEX_SETTINGS_IDS_H
#define NTVDMEX_SETTINGS_IDS_H

#define IDD_SETTINGS          200

#define IDC_S_HOSTCURSOR      201   /* checkbox: show the host arrow over the video   */
#define IDC_S_BLINKCURSOR     202   /* checkbox: blink the text-mode cursor           */
#define IDC_S_MSENS           203   /* edit:     mouse sensitivity, percent           */
#define IDC_S_DOSVER          204   /* combo:    reported MS-DOS version, "6.22"      */
#define IDC_S_PITPACE         205   /* checkbox: pace the PIT from a 1 kHz thread     */
#define IDC_S_UITICK          206   /* edit:     UI/present tick floor, milliseconds  */
#define IDC_S_DOSVER_NOTE     207   /* static:   why the version is a knob            */
#define IDC_S_DEFAULTS        208   /* button:   restore defaults                     */

#endif
