# Run the region-aware installer

These are custom builds, not an official PayloadLoaderInstaller release. The owner of the affected USA console confirmed successful Payload-Loader coldboot after restarting with the corrected installer. That result does not establish safety or compatibility on another console. Leave both Wii U Menu folders in place.

## Start with the read-only Diagnostic v2

Do this before any further installation or coldboot changes:

1. With the Wii U off, copy `artifacts/PayloadLoaderInstaller-RegionFix-Diagnostic.wuhb` into `SD:/wiiu/apps/`.
2. Safely eject the SD card, put it back in the console, and boot Aroma as you normally do.
3. Launch **PLI RegionFix Diagnostic** (not the installer). The heading must say **Diagnostic v2 - READ ONLY**. It runs its checks automatically; no Health & Safety launch proof is needed for these read-only checks. If it still says v1, the old binary is being launched.
4. Photograph the first report page. The app also appends a report to **SD:/regionfix-pli-diagnostic.log**, outside the existing backup directory.
5. Choose **Return to Wii U Menu**, then shut the console down before removing the SD card. Share that diagnostic log and/or first-page photo for analysis.

The diagnostic app has no install, restore, or coldboot-change menu. It exercises the same initial selection checks that failed in the installer, recording MCP result codes, region fields, title IDs/paths/types, file stat modes/sizes/errors, app.xml title identity, and supplementary raw IOSUHAX stat results. It only writes the diagnostic log on SD; it does not write SLC/MLC or log console serial fields. If SD saving fails, the report remains available on paginated screens (Up/Down to select an option, A to activate).

Version 1 identified a host-runtime compatibility bug: the bundled libiosuhax searched for unused device slots by comparing them with stdin, but the installed runtime uses NULL slots. Version 2 uses AddDevice/RemoveDevice, rejects duplicate mount names, preserves partial-mount ownership during cleanup, and blocks the installer if either mount fails. It also corrects a separate fstat handle bug in libiosuhax. Expected new results include `mount_fs SLC=0 errno=0 MLC=0 errno=0` and `INSTALLER SELECTION PASS: 000500101004E100`. Confirm the new log before changing coldboot.

For another console, complete the read-only checks and resolve any failures before following the steps below. Do not use the installer to work around a failed check.

## Before changing coldboot

1. Keep your existing `SD:/regionfix_backup/` backups. The recovery method used on this console was **UDPIH with recovery_menu**; ISFShax was never installed. Keep the equipment and files needed for that method available, and confirm you can still launch recovery_menu through UDPIH before changing coldboot. Do not assume an installed early-boot recovery mechanism is present.
2. Copy `artifacts/PayloadLoaderInstaller-RegionFix.wuhb` to `SD:/wiiu/apps/`. Its displayed name is **Payload-Loader Installer - RegionFix**. Avoid launching the original installer by mistake.
3. Optionally replace the old `regionfix.wuhb` with `artifacts/regionfix.wuhb` for inspection/backup and read-only `+` guidance. Do not choose region repair or restore: the logged USA region and coldboot values are already correct.
4. Start the patched installer and choose **Check**. Confirm it recognizes the USA Health & Safety installation. If there is an error, stop and record the exact message; do not bypass its checks or use an uninstall option as a workaround.

## Enable Payload-Loader coldboot

The upstream installer requires a session launched through a working Health & Safety Payload-Loader injection before enabling coldboot. This check has been retained and tightened to the selected region. Merely running Aroma through another entry point is not sufficient. See the [upstream usage requirements](https://github.com/wiiu-env/PayloadLoaderInstaller#usage).

1. If the USA Health & Safety injection is not installed or needs updating, use only the patched installer's normal **Install / Update** flow after reviewing its checks. That operation modifies USA Health & Safety; it does not modify the USA Wii U Menu. Do not reinstall if the installer already reports it current.
2. Shut down as instructed by the installer, start the console, launch the **USA Health & Safety** app to enter Payload-Loader/Aroma, then launch **Payload-Loader Installer - RegionFix** in that session. If Health & Safety does not load the intended environment, stop; coldboot is not ready to be enabled.
3. Choose **Check**, then **Boot options**. Verify the displayed values:

   | Field | Required value |
   |---|---|
   | Current coldboot (before this change) | `0005001010040100` |
   | Selected Menu | `0005001010040100` |
   | Selected H&S | `000500101004E100` |

4. Only with those IDs and all checks passing, choose **Switch to Payload-Loader**. This changes the coldboot target in SLC system.xml to USA Health & Safety. It does not move, uninstall, or modify either menu title.
5. On a success screen, use the offered shutdown option. Power on and check whether Payload-Loader starts correctly. Keep the SD card and environment files in place.

If the switch is not offered, record the displayed reason. A payload hash mismatch or missing launch proof is a stop condition, not permission to disable a safety check. If a write/restore/flush error says not to reboot, keep the console running and obtain recovery guidance before doing anything else.

## Undo and recovery

While the patched installer runs normally, **Boot options -> Switch back to Wii U Menu** selects the verified USA menu (`0005001010040100`). No EUR-folder undo is needed because this fix never moves the folder.

If normal coldboot fails but **UDPIH can still launch recovery_menu**, its **Set Coldboot Title -> Wii U Menu (USA)** is the direct recovery option for a wrong boot target. Its [documented options](https://github.com/GaryOderNichts/recovery_menu#options) also include wupserver, but this patch does not depend on an untested remote directory move. Recovery depends on UDPIH remaining usable and recovery_menu being able to read/write the relevant storage; it cannot be guaranteed from a PC build.

If inspection finds a menu was actually moved by an earlier tool, stop and assess the current filesystem before proceeding. The current RegionFix `+` screen deliberately does not attempt an automatic cross-folder restore.

## Expected outcome and reporting

Both menus remain listed by MCP. That is expected. The patched installer must select the USA menu irrespective of their ordering. The original installer remains affected by its one-entry selection bug, so do not use its Boot options on this console.

For a failed test, provide the exact patched-installer message and selected IDs. RegionFix's latest inspection can be saved in `SD:/regionfix_backup/regionfix.log`; do not publicly share the raw sysprod backups containing your serial.
