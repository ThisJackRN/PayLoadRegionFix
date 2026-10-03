# PayLoadRegionFix

A region-aware fork of PayloadLoaderInstaller, along with a read-only diagnostic app and the original RegionFix utility. It's for one narrow situation: a Wii U with **two regional Wii U Menus installed**, where the stock installer picks the wrong one when deciding what the console should boot.

**Status:** confirmed working on one USA console. The corrected installer enabled Payload-Loader coldboot, and it still worked after a restart. That's one console, not a compatibility guarantee.

If your Wii U is behaving normally, you don't need this.

## Background

When I was a kid, I installed Haxchi on my Wii U (Aroma wasn't a thing yet). I loved it, but of course I wanted the menu to look cooler, so I tried a UI swap with custom icons and all that. Like any kid, I found a video tutorial and followed it, confident that nothing could go wrong.

Everything went wrong.

I'd installed EU menu files instead of US ones and bricked the whole console. At the time, the only real fix was to open it up and solder, which I had no way of doing. So I did what most people did with their Wii U anyway: threw it on a shelf and forgot about it. (Why did we all buy a Wii U, again?)

Six years later I bought a second Wii U, and that got me wondering whether there was an easier way to unbrick the first one by now. There was: **UDPIH**, which takes a roughly $10 Raspberry Pi Pico. I ran the exploit, booted into recovery_menu, and got the console working again.

What I didn't know was that I'd only soft-fixed it.

I never checked what was actually installed, and the EU menu was still sitting there next to the US one. Later I installed Aroma and tried to use PayloadLoaderInstaller to boot straight into Payload-Loader. The installer grabbed the EU menu instead of the US one. It told me the boot option was already set and offered to **switch back to the Wii U Menu**. I clicked it, thinking "what could go wrong?"

**Bricked.** Again.

So without realizing it, I'd put my Wii U into a weird state with two system menus installed, one of which was quietly breaking the installer.

A reasonable person would have given up at this point. Maybe used the second Wii U they'd bought like an idiot, or just launched Health & Safety manually, which costs about one extra minute per boot.

Instead, after vibeslopping this together with AI, here we are.

This fixes one insanely niche situation. It's been tested on exactly one Wii U (the one from this story), so I have **zero** idea how it behaves anywhere else. But if your setup matches mine, it should work.

## The bug

My console had **both** the USA Wii U Menu (`0005001010040100`) and the EUR Wii U Menu (`0005001010040200`) installed. Region was USA, and coldboot pointed at the USA menu, so the console booted fine.

PayloadLoaderInstaller asks the system for "the" Wii U Menu, but only takes the first result. On my console, the EUR menu came back first. The installer then checks whether coldboot is already set up like this:

```cpp
systemXMLAlreadyPatched = (coldbootTitleId != systemMenuTitleId);
```

So it compared the USA menu (what coldboot actually pointed at) against the EUR menu (what it picked), saw they didn't match, and decided Payload-Loader was already installed. Then **"Switch back to Wii U Menu"** wrote the EUR menu into the boot config, and the console wouldn't boot.

Changing the region settings doesn't fix this. The region was already correct.

## The fix

The patched installer picks the right titles instead of trusting whichever one shows up first:

- Reads the console's region and gets the **full** list of installed menus.
- Picks the Wii U Menu and Health & Safety that match your region.
- Double-checks that those titles actually exist where they should.
- Only treats coldboot as "already done" if it really points at Health & Safety.
- Keeps all of the original installer's safety checks.

On my console, that means switching coldboot from the USA Wii U Menu (`0005001010040100`) to USA Health & Safety / Payload-Loader (`000500101004E100`). Both menus stay installed. Nothing gets deleted.

There's also a fix for the old bundled libiosuhax, which couldn't mount storage on the current Aroma runtime. That's what caused the "no compatible application" error on the first attempt.

The build produces three apps in `artifacts/`:

| File | What it does |
|---|---|
| `PayloadLoaderInstaller-RegionFix.wuhb` | The patched installer. |
| `PayloadLoaderInstaller-RegionFix-Diagnostic.wuhb` | Read-only. Checks your console and writes a log to `SD:/regionfix-pli-diagnostic.log`. |
| `regionfix.wuhb` | The original inspection tool. |

**Run the diagnostic first.** If it doesn't pick the menu and Health & Safety you expect, don't use the installer. Also, don't use the original installer's Boot options on a console like this, because it will make the same mistake again. See [RUN-ON-CONSOLE.md](RUN-ON-CONSOLE.md) for step-by-step instructions and recovery notes. Make sure you have backups and a working recovery method (like UDPIH + recovery_menu) before changing coldboot.

## Building

**Prebuilt `.wuhb` files are on the [Releases](../../releases) page.** You only need this section if you want to build them yourself.

You need devkitPro (devkitPPC, WUT, libmocha) and its MSYS2. The patched libiosuhax is built from `upstream/libiosuhax`, so you don't need to install it separately.

1. **Get the payload.** The installer embeds the exact payload from the official [PayloadLoaderInstaller v0.1.1](https://github.com/wiiu-env/PayloadLoaderInstaller/releases/tag/v0.1.1) release. Download that release's zip and extract the payload from the `.wuhb` inside it:

   ```powershell
   python tools/extract-tested-payload.py wiiu/apps/PayloadLoaderInstaller.wuhb upstream/PayloadLoaderInstaller/payload/root.rpx
   ```

   The script checks the payload's SHA1 (`1736574cf6c949557aed0c817eb1927e35a9b820`), and the build checks it again. Don't swap it out.

2. **Check devkitPro.** The build detects devkitPro from `DEVKITPRO` or the default Windows installation at `C:\devkitPro`. If you installed it elsewhere, set `DEVKITPRO` to that Windows directory. The devkitPro path must not contain spaces; the repo itself can live in a path with spaces. No script edits are needed.

3. **Build:**

   ```powershell
   ./build.cmd --no-pause
   ```

   Or just double-click `build.cmd`. It copies the source into a fresh staging directory under devkitPro's `msys2/tmp`, builds all three apps, and puts the `.wuhb` files in `artifacts/`.

## Credits

This builds on other people's work. I only patched it:

- [PayloadLoaderInstaller](https://github.com/wiiu-env/PayloadLoaderInstaller) by Maschell, rw-r-r-0644, GaryOderNichts, and contributors
- [PayloadFromRPX](https://github.com/wiiu-env/PayloadFromRPX)
- [recovery_menu](https://github.com/GaryOderNichts/recovery_menu)
- devkitPro/WUT, libmocha, and libiosuhax

This isn't an official release of any of these projects. Upstream licenses are kept in `upstream/`.

---

If this fixed your console too, I'd like to hear about it. If it didn't, please share the diagnostic log. Don't start deleting system titles to see what happens.
