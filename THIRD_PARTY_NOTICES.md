# Third-party notices

| Material | Licence | Used for |
|---|---|---|
| [LVGL](https://github.com/lvgl/lvgl) 8.3.11 | MIT, © LVGL Kft | UI library, `third_party/lvgl` (patched) |
| [cJSON](https://github.com/DaveGamble/cJSON) | MIT, © Dave Gamble and cJSON contributors | JSON parsing, `third_party/cJSON.*` |
| [SDL2 for webOS](https://github.com/webosbrew/SDL-webOS) | zlib, © Sam Lantinga | window, input and GPU rendering; `libSDL2-2.0.so.0` ships in the package |
| [ezinject](https://github.com/smx-smx/ezinject) | zlib, © Stefano Moioli | loads the Home key hook into the TV's input daemons; the `ezinject` binary and its CRT (linked into `libmytvhook.so`) ship in the package, built by `homehook/build.sh` |
| [lginputhook](https://github.com/Simon34545/lginputhook) / [inputhookpp](https://github.com/sundermann/inputhookpp) | BSD-3 / GPL-3.0 | the approach of `homehook/` (inject into lginput2 and micomservice, cut the key before `/dev/uinput`); no code taken |
| [Lumen](https://github.com/ammar027/lumen-webos) | MIT, © ammar027 | the design followed by myTV, its line-icon paths (`tools/make_icons.py`) and its weather city table (`assets/cities.c`) |
| [Geist](https://github.com/vercel/geist-font) typeface | SIL Open Font License 1.1, © The Geist Project Authors | `tools/geist-*.ttf` and the fonts generated from them in `assets/fonts.c` |
| Bundled wallpapers | public domain | `package/wallpapers/`: *View of Lake Pepin, Minnesota* (Robert S. Duncanson), *View near Newport* (John Frederick Kensett), *Sunset on the River Oise* (Charles-François Daubigny), *Landscape* (Jasper F. Cropsey), *Corn Fields* (Félix Vallotton) |
| Weather | [Open-Meteo](https://open-meteo.com), CC BY 4.0 | forecast data, fetched at run time |
| Location | [ip-api.com](https://ip-api.com) | approximate city when none is chosen, fetched at run time |

App icons shown on the home screen are read from the apps installed on the TV. They are not part of
this repository.
