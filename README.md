# System RPM

## Русский

**System RPM 1.6** — Windows-инструмент для автоматизации игровых маршрутов на сервере RPM. Приложение позволяет записать и воспроизводить маршрут с клавишами WASD, пробелом и движением мыши, а также настроить горячие клавиши для ручного и автоматического режимов.

В режиме стамины программа следит за выбранным игровым логом. При появлении сообщения об остановке и отдыхе для указанного персонажа она приостанавливает маршрут на заданное время, а затем продолжает его. Настройка длительности отдыха используется только в режиме стамины. В анти-АФК есть отдельная настройка интервала повтора, которая не меняет время отдыха стамины.

### Возможности

- Калибровка маршрута с пятисекундным обратным отсчётом.
- Запись движений мыши через Raw Input, а также WASD и пробела в режиме стамины.
- Ручной запуск и автоматическое воспроизведение маршрута.
- Обнаружение сообщения об отдыхе по имени персонажа в новых строках игрового лога.
- Отдельный таймер отдыха стамины и независимый интервал повтора анти-АФК.
- Иконка приложения встроена в EXE; отдельный файл аватара не нужен для значка программы.
- Отдельная калибровка и режим анти-АФК.

Приложение рассчитано на Windows. Используйте его только в соответствии с правилами сервера RPM.

## English

**System RPM 1.6** is a Windows utility for automating gameplay routes on the RPM server. It can record and replay routes using WASD, Space, and mouse movement, with configurable hotkeys for manual and automatic modes.

In stamina mode, the application watches a selected game log. When a new log entry reports that the configured character needs to stop and rest, the route pauses for the selected duration and then resumes. A separate anti-AFK mode replays calibrated mouse movement.

### Features

- Route calibration with a five-second countdown.
- Mouse capture through Raw Input, plus WASD and Space recording in stamina mode.
- Manual playback and automatic route replay.
- Rest-message detection by character name in new game-log entries.
- Independent anti-AFK repeat interval and stamina rest-duration settings.
- The application icon is embedded in the EXE, so no external avatar file is needed for the executable icon.
- Separate calibration and anti-AFK mode.

The application targets Windows. Use it in accordance with the RPM server rules.

## Структура проекта / Project structure

```text
assets/       Avatar images
build/        Temporary compiler outputs
resources/    Windows executable icon resource
src/          C++ application source
```

Настройки и файлы калибровки остаются рядом с `SystemRMP.exe`, чтобы существующие пользовательские данные продолжали загружаться.
Settings and calibration files stay next to `SystemRMP.exe` so existing user data remains available.
