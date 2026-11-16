# Translating Omni360

Each language is one text file here, named by its code: `fr.txt`, `de.txt`, `pt-br.txt`. Any language whose file is in this folder when Omni360 is built ships inside the XEX.

## Starting a language

1. Copy `TEMPLATE.txt` to `<code>.txt`.
2. Change the `@name` line to the language's name in that language, such as `@name = Français`. This is what the Settings list shows.
3. Set `@flag` to the flag shown beside it: `br`, `de`, `es`, `fr`, `gb`, `it`, `jp`, `kr`, `mx`, `nl`, `pl`, `pt`, `ru`, `se`, `tr`, `cn`, `tw`, `us` or `ca`. If yours isn't there, ask in your pull request, and it can be added to `tools/make_flags.py`.
4. Add `@clock = 24` if people who speak your language usually read the time as 21:41 rather than 9:41 PM. Without it the clock is 12-hour. Anyone can still change it under Settings → Language → Clock.
5. Add `@contributor = <your name or handle>`. Settings shows it beside the language, the way Aurora credits its translators.
6. Translate the text after each `=`. Leave the key before the `=` exactly as it is.

You don't have to finish before trying it. Any line you delete or leave blank is shown in English.

## Machine-translated languages

Some languages started as machine translations so Omni360 didn't launch with English only. Their files have `@machine = yes`, and Settings marks them "Machine translated". If you speak one of them, corrections are very welcome. Once someone has gone through the whole file, remove `@machine = yes` and add your `@contributor` line.

## The rules

- Save the file as UTF-8.
- `{1}`, `{2}`, `{3}` and `{4}` are filled in by Omni360, with things like a game's name, a number or a size. Keep every one the English line has, and add none. You can move them around if your grammar needs a different order. If a line's placeholders don't match the English, Omni360 shows that line in English and says so in the log.
- `\n` is a line break.
- Lines starting with `#` are comments.
- Some strings come in pairs, for one and for more than one, such as `COUNT_GAME` and `COUNT_GAMES`. If your language has more plural forms than that, translate them as best you can and say so in your pull request.
- Keep labels about as short as the English. Button and menu labels have little room, and long text gets cut off with "...".

## Trying it on the console

Put the file in a `Languages` folder beside Omni360's XEX, for example `Hdd1:\Apps\Omni360\Languages\fr.txt`, and pick it under **Settings → Language**. The file is read when you pick the language, so after editing it, pick another language and then pick yours again. `DebugInfo.txt` lists any lines that were left in English, and why.

## Checking it on a PC

```
python tools/make_languages.py --check fr
```

This lists the missing lines, any whose placeholders don't match, and any keys Omni360 doesn't have. A key Omni360 doesn't have is usually one that was renamed after you copied the template.

## For maintainers

- After adding or changing strings in `src/Strings.def`, run `python tools/make_languages.py --template` so translators have the new lines, then `python tools/make_en_ca.py` so English (Canada) picks them up too. That file is generated, so edit the script, not the file.
- Before a release build, run `python tools/make_languages.py` to compile this folder into `src/LanguagePacks.h`.
- `python tools/make_languages.py --pseudo` writes a fake language to `Languages/pseudo/xx.txt`: English with accents, stretched by a third, and in brackets. Copy it to the console's `Languages` folder and pick it. Any text without accents is still hardcoded, and any text whose closing `]` is cut off needs more room.
