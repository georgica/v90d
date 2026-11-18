# Putting v90d on an HT802, and taking it off again

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
   Firmware".
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
- **An HT801.** Two HT801 revisions (part numbers 9611003610A and
  9610003611C) are accepted, from what their flash contents say, but no
  unit has run v90d yet. On such a board a start only counts as good once
  the box has also received something on its network port. So plug the
  network cable in before the power: if the network does not work under
  v90d, or no cable is in for three starts in a row, the box goes back to
  the stock firmware by itself. Please open an issue and say how it went.
- **A different board revision.** The loader knows boards by their part
  number. On any other it does not start v90d and gives the box back to the
  stock firmware, so trying is safe. See [BOARDS.md](BOARDS.md), and please
  open an issue with the part number printed on the label.

No serial cable should be needed for any of this. The pulled-power cases
have been tested against models of the box's boot loader, not yet by pulling
the plug on a real one.
