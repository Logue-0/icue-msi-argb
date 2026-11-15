# icue-msi-argb

Control the ARGB headers of MSI 800-series motherboards from **Corsair iCUE 5**: one colour per header (smooth, ~25 fps) or per-LED through the motherboard's *Lighting Channel*.

Out of the box, iCUE's MSI plugin finds nothing to drive on these boards: MSI's Mystic Light SDK only exposes "SELECT ALL", without the direct mode the plugin needs. This project replaces the SDK DLL the plugin loads with a small proxy that talks to the board's LED controller itself.

Tested on: MAG X870 TOMAHAWK WIFI (MS-7E51), iCUE 5.52.93, MSI plugin (CueMsiPlugin.dll) 2.2.0, MSI Center installed alongside.

## What it does

- Announces each configured header to the iCUE plugin and drives it directly over HID (controller `0DB0:0076`).
- **Per-LED:** headers are concatenated into the plugin's Lighting Channel. iCUE colours are scaled to the real LED count (averaged when iCUE has more points than the strip), and the direction can be reversed.
- **Above 100 LEDs:** iCUE accepts at most 100 LEDs per plugin key range. With `keysplit=1` the proxy patches the plugin **in memory** (the signed file on disk is untouched) to spread LEDs over the three ranges iCUE accepts (100 + 50 + 10 = 160).
- A header whose LEDs are all the same colour uses the fast board packet; per-LED frames are slower (~93 ms each, about 10 fps for one header, ~3.5 fps if three animate at once). That is fine for static per-LED colours, but fast moving effects look stepped.
- When iCUE closes, the board's previous lighting is restored.

## Safety

The controller only ever receives:
- feature report `0x50` (board settings) with the "save to flash" byte forced to 0;
- feature report `0x51` (per-LED frame) for the JARGB_V2 / JAF areas;
- output report `0x01` with commands `0x82` (detect) and `0x84` (ARGB Gen2 on/off) for ports 0-3.

Nothing is written to flash, and firmware-update reports are never used. The board serial is checked against `board=` in the config before anything is sent.

## Install

1. Install iCUE 5 and its MSI plugin (Settings > Plugins).
2. Download the zip from Releases and extract it, or build `bin\MysticLight_SDK_x64.dll` yourself with `build.cmd` (Visual Studio 2022).
3. Run `install.ps1` (asks for admin). It keeps MSI's original DLL as `MysticLight_SDK_x64_real.dll`, installs the proxy and creates `C:\ProgramData\icue-msi-shim\config.txt` from `config.example.txt`.
4. Edit the config for your setup and (re)start iCUE.
5. Per-LED: in iCUE > motherboard > Lighting Setup choose *External RGB Light Strips* and add strips that match the `channel=` lines in order. Put your lighting layers on **Lighting Channel**, not on MOTHERBOARD LIGHTING.

Uninstall: `install.ps1 -Uninstall`.

iCUE updates can overwrite the proxy: run `install.ps1` again afterwards. If the MSI plugin version changes, `keysplit` turns itself off (iCUE is then limited to 100 LEDs) until the patch is updated.

## Config

See `config.example.txt`. In short:

| Line | Meaning |
|---|---|
| `channel=Label\|4\|lo\|LEDs\|iCUE LEDs[\|1]` | Per-LED header in the Lighting Channel; `lo` 0/1/2 = JARGB_V2_1/2/3; final `\|1` reverses the direction |
| `zone=Name\|4\|lo\|LEDs` | Single-colour header (one zone name must contain `JCORSAIR`) |
| `dev=` | Name shown in iCUE |
| `board=7E51` | Controller serial prefix allowed (empty = any; untested) |
| `keysplit=1` | Allow up to 160 channel LEDs |
| `debug=1` | Verbose log in `C:\ProgramData\icue-msi-shim\shim.log` |

## Known issues

- Every channel LED also appears as a "JCORSAIR nnn" button under MOTHERBOARD LIGHTING. It is cosmetic; leave that section without layers.
- The two Lian Li strips on one header are wired in parallel, so they always mirror each other (a hardware limit).
- Gen2 ARGB devices are switched to Gen1 mode while iCUE drives them, and switched back on exit. MSI Center can switch Gen2 back on at any time; the proxy re-asserts Gen1 every 3 s, so a Gen2 device may go dark for up to 3 s.

## How it works (short)

`CueMsiPlugin.dll` loads `MysticLight_SDK_x64.dll` from its own folder. The proxy forwards the SDK calls to MSI's real DLL, but fakes the "Direct All Sync" style and the LED names the plugin expects. It then turns `MLAPI_SetLedColorsSync` into HID packets for the controller. The protocol (`MSI_800sLed` in MSI Center's LEDKeeper2) was read from MSI's own software.

## Disclaimer

Not affiliated with Corsair or MSI. Use at your own risk.

## License

MIT
