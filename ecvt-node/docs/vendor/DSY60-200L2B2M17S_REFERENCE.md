# DSY60-200L2B2M17S motor, brake, and DSY-200C drive references

Installed system: `DSY60-200L2B2M17S` motor with its integrated holding brake and a `DSY-200C` CANopen drive.

## Vendor documents

| Component | Document | Source |
|---|---|---|
| Motor and brake | [local datasheet](DSY60-200L2B2M17S_Full_Datasheet.pdf) | [StepperOnline product page](https://www.stepperonline.ca/dsy-series-200w-canopen-dc-servo-motor-24-70vdc-0-637nm-w-brake-3000rpm-17-bit-incremental-encoder-ip65-dsy-c200l2b2-m17s.html) · [direct vendor PDF](https://files.omc-stepperonline.com/DSY60-200L2B2M17S_Full_Datasheet.pdf) |
| Drive and CANopen control | [local drive manual](DSY-C_Series_Low_Voltage_Servo_Drive_User_Manual_V1.0.pdf) | [ManualsLib copy](https://www.manualslib.com/manual/4123514/Stepperonline-Dsy-C-Series.html) · [direct vendor PDF](https://files.omc-stepperonline.com/DSY-C_Series_Low_Voltage_Servo_Drive_User_Manual_V1.0.pdf) |
| CANopen object dictionary | `DSY-C.EDS` | [vendor product downloads](https://www.stepperonline.ca/dsy-series-200w-canopen-dc-servo-motor-24-70vdc-0-637nm-w-brake-3000rpm-17-bit-incremental-encoder-ip65-dsy-c200l2b2-m17s.html) |

The original PDFs were supplied locally on 2026-11-20 and copied here after PDF validation. SHA-256: `b619a9b311454c5b37b456219a1aed1990893b1f8954e65753841a14fe3244f2` (motor datasheet), `f0e02fc2539842737a90055519a26f36889f50e37cf8acabd80b46a39a10a6ed` (drive manual).

## Motor and brake values

| Item | Value |
|---|---:|
| Rated power | 200 W |
| Rated motor voltage | 48 VDC |
| Rated / peak torque | 0.637 / 1.91 N·m |
| Rated current | 5.2 A |
| Rated speed | 3000 rpm |
| Encoder | 17-bit single-turn magnetic incremental |
| Brake holding torque | 1.52 N·m |
| Brake supply | 24 VDC |
| Brake power / current | 7.3 W / 0.32 A |

## Brake control facts from the drive manual

- The brake holds the shaft when the drive is not running; release is energized.
- The DSY drive's second digital output must be assigned `FunOUT.11: BK` for drive-managed brake control.
- `P02.22` selects the DO2 function; `P02.32` controls its logic inversion.
- `P00.14` controls BK ON delay, `P00.15` controls BK OFF delay, and `P00.16` sets the speed threshold for BK OFF; these apply only once BK is assigned.
- Profile-torque mode uses the normal CiA-402 controlword state sequence (`0x0006`, `0x0007`, `0x000F`).

Do not infer undocumented manufacturer parameter CANopen indexes from the keypad parameter numbers. See [DSY_BRAKE_CONTROL.md](../DSY_BRAKE_CONTROL.md) for the observed bench behavior and the project's guarded test commands.
