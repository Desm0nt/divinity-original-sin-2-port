Divinity Original Sin 2 Mobile - put your game files here
=========================================================

Put these things into this folder (any file names are fine):

  1. Divinity: Original Sin 2 - Definitive Edition base game   title 010027400CDC6000 v0         NSP, NSZ, XCI or XCZ
  2. Divinity: Original Sin 2 update v786432                   title 010027400CDC6800 v786432   NSP, NSZ, XCI or XCZ
                                                               (an XCI that already contains this update counts as both 1 and 2)
  3. prod.keys                                                 the keys of YOUR OWN console (dumped with Lockpick_RCM)

Optional - only if you also own the game for iPad / iPhone:

  4. its .ipa file (e.g. divinity-original-sin-2_<version>.ipa). Setup reads only its interface movies from it and draws
     the iPad's on-screen buttons into the console HUD (the button column on the right edge, the panels' button rows).
     Without it the game is fully playable with the console interface, touch on the menus and the on-screen pad.

Split dumps from a FAT32 SD card work too: the folder with 00, 01, ... parts, or name.xc0 + name.xc1 ...

Then run setup:
  Windows:        double-click setup.bat (one folder up)
  Linux / macOS:  ./setup.sh  (one folder up)

It prepares work/DivinityOriginalSin2Mobile with romfs, dm/out, dm/shaders, astc and drivers; copy that folder to the
phone as Download/DivinityOriginalSin2Mobile (setup prints the adb commands, or use: setup push). The astc folder is only
needed on phones without BC textures (Mali, PowerVR, Xclipse).

Your files are only read; nothing is copied, changed or uploaded. Everything in this folder except this
README is ignored by Git. Dump the game and the keys from a Switch you own: they are never shared by this
project. Step-by-step guide: docs/INSTALL.md (Russian: docs/INSTALL.ru.md).

----------------------------------------------------------------------------------------------------------

Divinity Original Sin 2 Mobile - положите сюда файлы игры
=========================================================

Положите в эту папку (имена файлов любые):

  1. Базовая игра Divinity: Original Sin 2 - Definitive Edition   title 010027400CDC6000 v0         NSP, NSZ, XCI или XCZ
  2. Обновление v786432                                           title 010027400CDC6800 v786432   NSP, NSZ, XCI или XCZ
                                                                  (XCI, в котором уже есть это обновление, считается сразу за 1 и 2)
  3. prod.keys                                                    ключи ВАШЕЙ консоли (снятые через Lockpick_RCM)

Необязательно - только если у вас есть ещё и версия для iPad / iPhone:

  4. её файл .ipa (например divinity-original-sin-2_<версия>.ipa). Setup берёт из него только фильмы интерфейса и рисует
     экранные кнопки iPad в консольном HUD (колонка кнопок у правого края, ряды кнопок в панелях). Без него игра
     полностью проходима: консольный интерфейс, касание в меню и экранный пад.

Подходят и разбитые дампы с карты FAT32: папка с частями 00, 01, ... или name.xc0 + name.xc1 ...

Затем запустите установку:
  Windows:        двойной щелчок по setup.bat (на уровень выше)
  Linux / macOS:  ./setup.sh  (на уровень выше)

Получится папка work/DivinityOriginalSin2Mobile (romfs, dm/out, dm/shaders, astc, drivers); скопируйте её на телефон как
Download/DivinityOriginalSin2Mobile (установка напечатает команды adb, или: setup push). Папка astc нужна только
телефонам без BC-текстур (Mali, PowerVR, Xclipse).

Файлы только читаются: ничего не копируется, не меняется и не отправляется. Подробная инструкция:
docs/INSTALL.ru.md (по-английски: docs/INSTALL.md).
