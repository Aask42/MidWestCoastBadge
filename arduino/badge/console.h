// console.h - serial provisioning console.
//
// Typing a WPA2 passphrase and a broker IP on a 240px on-screen keyboard is
// miserable, and it is the first thing anyone has to do with a new badge. This
// lets a badge be provisioned over USB in one line instead.
//
// Commands (case-insensitive, newline-terminated):
//   wifi <ssid> <pass>              set credentials and join
//   mqtt <host> <port> [user] [pw]  set broker and reconnect
//   name <text>                     set the nametag
//   show                            print current config
//   help
//
// Compiled out entirely when DEBUG_SERIAL is 0, so a production build has no
// serial control surface at all.

#pragma once

#include <Arduino.h>

void consoleTick();
