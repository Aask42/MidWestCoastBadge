#!/usr/bin/env python3
"""Pygame production station for building and flashing DEF CON 34 badges."""

import argparse
import concurrent.futures
import os
import queue
import threading
import time
from types import SimpleNamespace

import pygame

import mass_flash


WIDTH, HEIGHT = 1120, 720
BG = (8, 10, 14)
PANEL = (16, 20, 25)
PANEL_2 = (23, 28, 34)
WHITE = (239, 242, 236)
MUTED = (126, 141, 145)
CYAN = (30, 218, 204)
AMBER = (255, 181, 55)
RED = (242, 72, 77)
GREEN = (94, 224, 137)
GRID = (27, 67, 72)


class FlashController:
    def __init__(self):
        self.events = queue.Queue()
        self.stop_event = threading.Event()
        self.thread = None

    @property
    def running(self):
        return bool(self.thread and self.thread.is_alive())

    def emit(self, kind, payload):
        self.events.put((kind, payload))

    def start(self, once, build, storage, reflash):
        if self.running:
            return
        self.stop_event.clear()
        self.thread = threading.Thread(
            target=self._run,
            args=(once, build, storage, reflash),
            daemon=True,
        )
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        self.emit("log", ("Stopping after active writes finish...", AMBER))

    def _run(self, once, build, storage, reflash):
        def report(message, colour):
            tone = WHITE
            if colour == mass_flash.GREEN:
                tone = GREEN
            elif colour == mass_flash.YELLOW:
                tone = AMBER
            elif colour == mass_flash.RED:
                tone = RED
            elif colour == mass_flash.DIM:
                tone = MUTED
            self.emit("log", (message, tone))

        mass_flash.set_reporter(report)
        args = SimpleNamespace(
            build=build,
            storage=storage,
            forget=False,
            reflash=reflash,
            probe=True,
            settle=1.5,
            baud=921600,
            timeout=240,
            jobs=4,
            grace=3.0,
            poll=0.5,
        )
        pool = None
        try:
            self.emit("state", "BUILDING" if build else "LOADING IMAGE")
            plan = mass_flash.build_plan(args)
            flasher = mass_flash.Flasher(plan, args)
            pool = concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs)
            futures = {}
            present = mass_flash.ports_now()
            watcher = mass_flash.PortWatcher([] if once else present, args.grace)

            if once:
                self.emit("state", "FLASHING CONNECTED")
                if not present:
                    raise RuntimeError("No compatible serial devices are connected.")
            else:
                self.emit("state", "LIVE BAY ARMED")
                report("Live bay armed. Plug in the next badge.", mass_flash.YELLOW)
                if present:
                    report("Devices already connected are ignored until replugged.",
                           mass_flash.DIM)

            while not self.stop_event.is_set():
                for port, future in list(futures.items()):
                    if future.done():
                        watcher.release(port)
                        del futures[port]

                for port in watcher.poll(mass_flash.ports_now()):
                    futures[port] = pool.submit(flasher.handle, port)

                self.emit("counts", (flasher.ok, flasher.failed,
                                     flasher.skipped))
                if once and not futures:
                    break
                time.sleep(args.poll)

            if self.stop_event.is_set() and futures:
                self.emit("state", "FINISHING ACTIVE WRITES")
            pool.shutdown(wait=True)
            pool = None
            self.emit("counts", (flasher.ok, flasher.failed, flasher.skipped))
            self.emit("state", "READY" if flasher.failed == 0 else "CHECK FAILURES")
        except (RuntimeError, SystemExit, OSError) as error:
            self.emit("log", (str(error), RED))
            self.emit("state", "BLOCKED")
        finally:
            if pool:
                pool.shutdown(wait=True)
            mass_flash.set_reporter(None)
            self.emit("done", None)


class Button:
    def __init__(self, label, action, accent=CYAN):
        self.label = label
        self.action = action
        self.accent = accent
        self.rect = pygame.Rect(0, 0, 0, 0)

    def draw(self, surface, font, mouse, enabled=True):
        hover = enabled and self.rect.collidepoint(mouse)
        fill = self.accent if hover else PANEL_2
        ink = BG if hover else (WHITE if enabled else MUTED)
        pygame.draw.rect(surface, fill, self.rect, border_radius=4)
        pygame.draw.rect(surface, self.accent if enabled else GRID, self.rect,
                         1, border_radius=4)
        text = font.render(self.label, True, ink)
        surface.blit(text, text.get_rect(center=self.rect.center))


class StationApp:
    def __init__(self, screenshot=None):
        pygame.init()
        pygame.display.set_caption("MIDWESTCOAST Flash Grid")
        self.screen = pygame.display.set_mode((WIDTH, HEIGHT), pygame.RESIZABLE)
        self.clock = pygame.time.Clock()
        self.controller = FlashController()
        self.screenshot = screenshot
        self.font_small = pygame.font.SysFont("Avenir Next Condensed", 17)
        self.font_body = pygame.font.SysFont("Avenir Next Condensed", 21)
        self.font_button = pygame.font.SysFont("Avenir Next Condensed", 19, bold=True)
        self.font_head = pygame.font.SysFont("Avenir Next Condensed", 42, bold=True)
        self.font_number = pygame.font.SysFont("Menlo", 34, bold=True)
        self.logs = [("Station ready. Build an image or arm the live bay.", MUTED)]
        self.state = "READY"
        self.counts = (0, 0, 0)
        self.build = True
        self.storage = True
        self.reflash = False
        self.credits = False
        self.running = True
        self.buttons = [
            Button("ARM LIVE BAY", lambda: self.start(False), CYAN),
            Button("FLASH CONNECTED", lambda: self.start(True), AMBER),
            Button("STOP", self.controller.stop, RED),
            Button("CREDITS", self.toggle_credits, MUTED),
        ]

    def start(self, once):
        self.logs = []
        self.counts = (0, 0, 0)
        self.controller.start(once, self.build, self.storage, self.reflash)

    def toggle_credits(self):
        self.credits = not self.credits

    def add_log(self, message, colour):
        for line in str(message).splitlines() or [""]:
            self.logs.append((line, colour))
        self.logs = self.logs[-18:]

    def drain_events(self):
        while True:
            try:
                kind, payload = self.controller.events.get_nowait()
            except queue.Empty:
                return
            if kind == "log":
                self.add_log(*payload)
            elif kind == "state":
                self.state = payload
            elif kind == "counts":
                self.counts = payload

    def draw_grid(self, width, height):
        horizon = int(height * 0.36)
        pygame.draw.line(self.screen, GRID, (0, horizon), (width, horizon), 1)
        center = width // 2
        for x in range(-width, width * 2, 80):
            pygame.draw.line(self.screen, GRID, (center, horizon), (x, height), 1)
        y = horizon + 16
        gap = 16
        while y < height:
            pygame.draw.line(self.screen, GRID, (0, y), (width, y), 1)
            gap = int(gap * 1.28)
            y += gap

    def draw_toggle(self, x, y, label, value):
        rect = pygame.Rect(x, y, 48, 24)
        pygame.draw.rect(self.screen, CYAN if value else PANEL_2, rect,
                         border_radius=4)
        knob_x = rect.right - 20 if value else rect.left + 4
        pygame.draw.rect(self.screen, BG if value else MUTED,
                         (knob_x, y + 4, 16, 16), border_radius=3)
        text = self.font_small.render(label, True, WHITE)
        self.screen.blit(text, (rect.right + 10, y + 2))
        return rect

    def draw(self):
        width, height = self.screen.get_size()
        self.screen.fill(BG)
        self.draw_grid(width, height)
        pygame.draw.rect(self.screen, (8, 10, 14, 224), (0, 0, width, height))

        left = max(28, int(width * 0.04))
        top = 28
        title = self.font_head.render("MIDWESTCOAST // FLASH GRID", True, WHITE)
        self.screen.blit(title, (left, top))
        sub = self.font_small.render(
            "DEVICE PROVISIONING  /  DEF CON 34  /  2026", True, CYAN)
        self.screen.blit(sub, (left + 2, top + 50))

        status_color = GREEN if self.state == "READY" else AMBER
        if self.state in ("BLOCKED", "CHECK FAILURES"):
            status_color = RED
        status = self.font_body.render(self.state, True, status_color)
        self.screen.blit(status, (width - status.get_width() - left, top + 10))

        content_top = 112
        right_w = 310
        gap = 24
        log_rect = pygame.Rect(left, content_top,
                               width - left * 2 - right_w - gap, height - 180)
        control_rect = pygame.Rect(log_rect.right + gap, content_top,
                                   right_w, height - 180)
        pygame.draw.rect(self.screen, PANEL, log_rect, border_radius=4)
        pygame.draw.rect(self.screen, PANEL, control_rect, border_radius=4)
        pygame.draw.rect(self.screen, GRID, log_rect, 1, border_radius=4)
        pygame.draw.rect(self.screen, GRID, control_rect, 1, border_radius=4)

        self.screen.blit(self.font_small.render("STATION FEED", True, CYAN),
                         (log_rect.x + 18, log_rect.y + 16))
        y = log_rect.y + 50
        for line, colour in self.logs[-16:]:
            max_chars = max(12, (log_rect.width - 36) // 10)
            shown = line if len(line) <= max_chars else line[:max_chars - 1] + "~"
            self.screen.blit(self.font_small.render(shown, True, colour),
                             (log_rect.x + 18, y))
            y += 26

        card_y = control_rect.y + 18
        labels = (("FLASHED", self.counts[0], GREEN),
                  ("FAILED", self.counts[1], RED),
                  ("SKIPPED", self.counts[2], MUTED))
        card_w = (control_rect.width - 48) // 3
        for index, (label, number, colour) in enumerate(labels):
            x = control_rect.x + 12 + index * (card_w + 12)
            rect = pygame.Rect(x, card_y, card_w, 82)
            pygame.draw.rect(self.screen, PANEL_2, rect, border_radius=4)
            number_text = self.font_number.render(str(number), True, colour)
            self.screen.blit(number_text,
                             number_text.get_rect(center=(rect.centerx, rect.y + 34)))
            label_text = self.font_small.render(label, True, MUTED)
            self.screen.blit(label_text,
                             label_text.get_rect(center=(rect.centerx, rect.y + 66)))

        button_y = card_y + 108
        mouse = pygame.mouse.get_pos()
        for button in self.buttons:
            button.rect = pygame.Rect(control_rect.x + 16, button_y,
                                      control_rect.width - 32, 44)
            enabled = not self.controller.running or button.label in ("STOP", "CREDITS")
            button.draw(self.screen, self.font_button, mouse, enabled)
            button_y += 56

        toggle_y = control_rect.bottom - 92
        self.build_rect = self.draw_toggle(control_rect.x + 18, toggle_y,
                                           "BUILD FIRST", self.build)
        self.storage_rect = self.draw_toggle(control_rect.x + 18, toggle_y + 30,
                                             "INCLUDE ART", self.storage)
        self.reflash_rect = self.draw_toggle(control_rect.x + 160, toggle_y,
                                             "REFLASH", self.reflash)

        ports = len(mass_flash.ports_now())
        footer = (f"{ports} compatible port{'s' if ports != 1 else ''} connected"
                  "  //  Amelia Wietting  //  Aask Questions  //  aask.ltd")
        self.screen.blit(self.font_small.render(footer, True, MUTED),
                         (left, height - 42))

        if self.credits:
            self.draw_credits(width, height)

        for y in range(0, height, 4):
            pygame.draw.line(self.screen, (0, 0, 0), (0, y), (width, y), 1)
        pygame.display.flip()

    def draw_credits(self, width, height):
        shade = pygame.Surface((width, height), pygame.SRCALPHA)
        shade.fill((0, 0, 0, 210))
        self.screen.blit(shade, (0, 0))
        rect = pygame.Rect(width // 2 - 260, height // 2 - 170, 520, 340)
        pygame.draw.rect(self.screen, PANEL, rect, border_radius=4)
        pygame.draw.rect(self.screen, CYAN, rect, 2, border_radius=4)
        lines = [
            ("MIDWESTCOAST 2026", self.font_head, WHITE),
            ("Badge firmware + production tools", self.font_body, MUTED),
            ("AMELIA WIETTING", self.font_head, CYAN),
            ("AKA Aask Questions", self.font_body, AMBER),
            ("aask.ltd", self.font_body, WHITE),
            ("Click anywhere to return", self.font_small, MUTED),
        ]
        y = rect.y + 30
        for text, font, colour in lines:
            rendered = font.render(text, True, colour)
            self.screen.blit(rendered,
                             rendered.get_rect(center=(rect.centerx, y + 18)))
            y += 52

    def handle_click(self, position):
        if self.credits:
            self.credits = False
            return
        if not self.controller.running:
            if self.build_rect.collidepoint(position):
                self.build = not self.build
            elif self.storage_rect.collidepoint(position):
                self.storage = not self.storage
            elif self.reflash_rect.collidepoint(position):
                self.reflash = not self.reflash
        for button in self.buttons:
            enabled = not self.controller.running or button.label in ("STOP", "CREDITS")
            if enabled and button.rect.collidepoint(position):
                button.action()
                return

    def run(self):
        frames = 0
        while self.running:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    self.controller.stop()
                    self.running = False
                elif event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE:
                    if self.credits:
                        self.credits = False
                    else:
                        self.controller.stop()
                        self.running = False
                elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                    self.handle_click(event.pos)
            self.drain_events()
            self.draw()
            frames += 1
            if self.screenshot and frames == 2:
                pygame.image.save(self.screen, self.screenshot)
                self.running = False
            self.clock.tick(30)
        pygame.quit()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screenshot", help="render one frame to a PNG and exit")
    args = parser.parse_args()
    StationApp(args.screenshot).run()


if __name__ == "__main__":
    main()