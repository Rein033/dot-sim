*By completing this PR sufficiently, you help us review it quicker and keep the release notes honest.*

### Checklist
1. [ ] A title in the commit format, `type(scope): Summary` (*eg. "feat(chips): Model the QMI8658A's FIFO"*)
2. [ ] Related links below (*eg. the issue this PR closes*)
3. [ ] The README and the CHANGELOG updated if the change touches what they say
4. [ ] `tools/host_tests.sh` run, and green
5. [ ] A change that the firmware sees run in esp-emu: `pytest -m emu tests` green, or the tests you ran named below

*This entire section above can be deleted if all items are checked.*

-----------
## Description of Change
Please describe the proposed change and its impact.

## Test Scenarios
Please describe on what computer and with what firmware this was tested, and how.

(*eg. macOS 15 on an M2; host suites and the emulator suite against kode-sdk develop; kodeOS and display_lvgl run by hand, screenshots attached*)

## Related links
Please provide links to related issues, PRs etc.

(*eg. Closes #number of issue*)
