# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

import os

from pbl import emulator

from .core import Runner, RunnerCaps, RunnerError

# Backs the external flash; kept across runs, like a watch's.
FLASH_FILE = "pebbleos-flash.bin"


class NativeRunner(Runner):
    name = "native"

    def __init__(self, cfg, bt_hci=None):
        super().__init__(cfg)
        self.bt_hci = bt_hci

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={"run"})

    @classmethod
    def supports(cls, cfg):
        return not cfg.build.config.CONFIG_POSIX_EMSCRIPTEN

    @classmethod
    def do_create(cls, cfg, args):
        # --bt-hci is the qemu runner's, shared with this one.
        return cls(cfg, bt_hci=getattr(args, "bt_hci", None))

    def do_run(self, command):
        build = self.cfg.build
        program = build.join(build.project)
        if not self.cfg.dry_run and not os.path.isfile(program):
            raise RunnerError(f"no {program}; run 'pbl build' first")

        hci_uart = bool(build.config.CONFIG_BT_HCI_UART)
        if self.bt_hci and not hci_uart:
            raise RunnerError("--bt-hci needs a build with CONFIG_BT_HCI_UART=y")

        argv = [
            program,
            "-f", build.join(FLASH_FILE),
            "-p", str(emulator.PEBBLE_TOOL_PORT),
        ]  # fmt: skip
        if self.bt_hci:
            argv += ["-b", self.bt_hci]
        return self.run_program(argv + list(self.cfg.extra_args))
