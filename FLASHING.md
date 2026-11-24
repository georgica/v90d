# Putting v90d on an HT802, and taking it off again

(The Obihai OBi200 goes a different way: see
[The Obihai OBi200](#the-obihai-obi200) at the end.)

Tried on: HT802 hardware V2.5A (part number 9610003725A) with stock firmware
1.0.65. Read "If something goes wrong" before you start.

**Not the HT802V2.** That is a newer product with signed firmware
(`SecureStatus: 3`); its upgrader refuses v90d, so the upload steps below do
not work on it. Whether another route exists is under investigation. See
[BOARDS.md](BOARDS.md).

## Install

1. Get `v90d-ht802fw.bin` (from the releases page, or build it: README.md).
2. Power the box and **wait two minutes**. In its first minute the stock
   firmware checks for provisioning and refuses uploads with "Device already
   has another provision running!". Nothing is written in that case; just
   try again later.
3. Log in to the stock web page as `admin`.
4. Advanced Settings, Upgrade and Provisioning, "Upload Firmware: Upload
   from local directory". Choose `v90d-ht802fw.bin`, press "Upload
   Firmware". If the page answers at once with "Your firmware file is
   invalid", the stock firmware is checking the file's name (the HT801's
   1.0.65.3 does): rename the file to `ht802fw.bin` and choose it again.
5. The page says the device is processing the firmware. Leave the power on.
   After two to three minutes the box restarts by itself.
6. Open the same address again. The page is now "Nestaliga". Log in with
   `admin` / `admin` and change the password on the System page.

The box keeps asking for its address by DHCP on the same Ethernet port.

Plug a modem into PHONE 1 or PHONE 2 and dial the number shown on the Line
page, with any PPP user from the Users page (the factory user is `guest` /
`guest`). It should connect.

## What the install does

The HT802's flash holds two complete copies of the firmware, and the stock
upgrade always writes the copy that is not running. v90d goes into that
copy. The stock firmware you were running stays in the other one, untouched.

## Back to the stock firmware

On the v90d pages: Firmware, "Back to the stock firmware", tick the box,
confirm. The box restarts into the stock firmware with the settings and the
password it had. To return to v90d, upload the file again (the v90d settings
are not kept across that).

## Upgrading v90d

On the v90d pages: Firmware, upload the new `v90d-ht802fw.bin`. Settings
are kept.

The box can also tell you when there is a newer release and fetch it for
you. On the Settings page, under System, turn "Check for updates" on (it is
off to start with, so the box contacts no one unless you ask). Once a day
the box then asks GitHub whether a newer v90d release exists; if one does,
the Firmware page shows "Update available" with the version, a link to its
release notes, and a "Download and install" button. The button makes the
box download that release, check it in full (a damaged file, or one for
another box, is refused and nothing changes), write the spare flash copy
and restart into it, the same as uploading the file by hand. Nothing is
ever installed without that click, and the stock firmware and the settings
are kept. It fetches only the project's own release over HTTPS; there is
no signature on the image yet, so the trust is the same as downloading the
file from the releases page and uploading it yourself.

## If something goes wrong

- **The box does not come back with v90d.** Every start of v90d is counted
  before it is made, and a start only counts as good after the system has
  run for a minute. After three starts in a row that did not get that far,
  the loader gives up and the box starts the stock firmware again. If the
  box seems dead, unplug it and plug it in again, up to three times, waiting
  two minutes each time.
- **The power went during the install or an upgrade.** The box starts the
  stock firmware (an incomplete copy is never started). Upload the file
  again.
- **An HT801.** Upload `v90d-ht801fw.bin`, renamed to `ht801fw.bin` first
  (the stock page checks the filename before sending). Three part numbers
  are accepted (9610003610A, 9611003610A and 9610003611C); V1.0A
  (9610003610A) has been confirmed by its owner. On such a board a start
  only counts as good once the box has also received something on its
  network port. So plug the network cable in before the power: if the
  network does not work under v90d, or no cable is in for three starts in
  a row, the box goes back to the stock firmware by itself. Once v90d runs,
  its Firmware page takes only the HT801 file (the HT802 one is refused),
  and the update check fetches the HT801 file by itself. Please open an
  issue and say how it went.
- **A different board revision.** The loader knows boards by their part
  number. On any other it does not start v90d and gives the box back to the
  stock firmware, so trying is safe. See [BOARDS.md](BOARDS.md), and please
  open an issue with the part number printed on the label.

No serial cable should be needed for any of this. The pulled-power cases
have been tested against models of the box's boot loader, not yet by pulling
the plug on a real one.

## The Obihai OBi200

Tried on: OBi200 hardware 1.4, from the community firmware
8680EX-arrynrob13. Not the OBi202 (two phone ports): v90d does not run there
yet.

### What you need

- **An OBi200 that takes firmware the maker did not sign.** `v90d-obi200.fw`
  is the community firmware (arrynrob13, from obifirmware.com) with v90d in
  it, so it goes on wherever that firmware goes, by the community's route.
  Follow their install instructions and upload `v90d-obi200.fw` where they
  say arrynrob13. In short:
  - **stock before build 5853:** upload it directly;
  - **5853 to 6258:** install their `bypass-uboot-mod` first;
  - **6259 and later:** install stock 5921EX, then `bypass-uboot-mod`, then
    this file (`bypass-uboot-mod` straight from 6259 or later ends in
    recovery mode).

  A box already on the community firmware is ready. That is the path tested
  here; the stock routes have not been tried with v90d yet.
- **The community firmware file** (`OBi2xx-3-2-2-8680EX-arrynrob13.fw`, md5
  `b438e5161220e1e7c161e005de1d6bb6`). Keep it: it is your way back.

### Install

1. Get `v90d-obi200.fw` from the releases page (or build it:
   [fw/obi/README.md](fw/obi/README.md)).
2. On the OBi's own web page (System Management, Device Update), upload
   `v90d-obi200.fw` as you would any firmware.
3. Leave the power on. The box writes the flash and restarts by itself. It
   then starts twice over: first the maker's kernel, then v90d's own, which
   the maker's starts from memory. Give it about three minutes.
4. Open the address the box gets by DHCP (your router's list of clients
   shows it). The page is now "Nestaliga". Log in with `admin` / `admin` and
   change the password on the System page.

Plug a modem into the PHONE port and dial the number shown on the Line page,
with any PPP user from the Users page (the factory user is `guest` /
`guest`).

### What the install does

Only the box's `/obi` part of the flash is replaced (the maker's
application). The boot loader, the maker's kernel, the system files and the
recovery mode stay as they are. At every start the maker's kernel boots
first and starts v90d's own kernel, which v90d keeps in `/obi`. There is no
SSH under v90d's kernel, as on the HT802.

### Upgrading v90d

On the v90d pages: Firmware, upload the new `v90d-obi200.fw`. The page checks
the whole file, writes only what differs from the flash (normally `/obi`
alone) and restarts the box. Settings are kept.

### Back to the stock firmware

On the v90d pages: Firmware, upload the community firmware file itself. It
differs from the flash in `/obi` only, so that is all that is written, and
the box restarts into the maker's application with its old settings.

### If something goes wrong

- **v90d's kernel does not come up.** The box restarts on the maker's kernel
  and tries again. After two starts in a row that did not come up, it stays
  on the maker's kernel, where v90d runs too, until the power is cut. So the
  pages are back either way; please open an issue and say what happened.
- **The power went while the flash was written**, or the box cannot start
  for another reason. It comes up in the maker's **recovery mode**: a plain
  page titled "OBi202 Firmware Update" at the box's address. It takes any
  firmware file: upload `v90d-obi200.fw` again, or the community firmware
  to go back. v90d never writes the boot loader or the recovery images, so
  that mode is always there.
- **The page says the write failed.** The system that runs is in memory and
  still whole: upload the file again before you restart the box.

No serial cable should be needed for any of this.
