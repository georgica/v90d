# Contributing

Anyone may contribute. A few rules keep the project what it is.

**Originality.** The image contains Linux, busybox and a C library as they
are, and nothing else that was not written for this project. Please do not
copy code from other modem, compression or PPP implementations (spandsp,
pppd, D-Modem, vendor sources), and do not add vendor binaries or firmware
dumps to the tree. Write from the ITU-T recommendations and the RFCs. Facts
learned by studying the stock firmware belong in `HARDWARE.md`, in your own
words.

**Code.** Portable C99. Fixed-point only in the signal path. No allocation
after start-up. No globals: every object is a struct its caller owns.

**Tests.** Write the test first and see it fail. A change is ready when
`make check` passes, and `make check-arm` for anything that runs on the box.
Changes to the firmware file, the flash tools or the loader also need
`make -C fw test-release`. Do not claim something works on hardware without
the output that shows it; put that in `PROGRESS.md`.

**Flash safety.** Nothing may write to the flash without a tested way back.
If you change the loader or the upgrade path, say in the pull request how a
box recovers when the power goes at the worst moment.

**Reports from other hardware** are very welcome: board revision and part
number, stock firmware version, modem model, and what happened.

By contributing you agree that your contribution is licensed under
GPL-2.0-only, like the rest of the tree.
