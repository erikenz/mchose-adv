# Contributing to mchose-adv

Contributions are very welcome! Whether you are adding support for another MCHOSE keyboard model, reporting protocol observations, or improving packaging, thank you for helping.

## Adding Support for New Keyboards

If you have another MCHOSE hall-effect keyboard (e.g. Jet 75, Ace 68 Air/Turbo/GT, Zero 75X, Mix 87):

1. **Find your USB ID:**
   ```sh
   lsusb | grep -iE 'mchose|3837|41e4'
   ```
2. **Capture Interface 2 hidraw output:**
   Follow [`docs/PROTOCOL.md` §5](docs/PROTOCOL.md#5-reproducing-the-observation) to confirm that advanced keys (SOCD/RS) are emitted on USB interface 2 in the same 16-byte report format.
3. **Add the device to `src/mchose-adv.c`:**
   Add an entry to the `known[]` table in [`src/mchose-adv.c`](src/mchose-adv.c).
4. **Update `packaging/70-mchose-adv.rules`:**
   Add a udev rule matching your keyboard's vendor and product ID.
5. **Update tests and documentation:**
   Add the IDs to `tests/test_keymap.c` and document your findings.

## Development & Testing

- Build the project:
  ```sh
  make
  ```
- Run the test suite:
  ```sh
  make test
  ```
- Code must compile without warnings under `-Wall -Wextra`.

## Submitting Pull Requests

- Keep PRs focused on a single change or fix.
- Fill out the PR template describing your hardware and test results.
- Ensure `make test` passes.
