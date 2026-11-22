# Player text

Text players read in the mod's screens comes from one table,
`data/strings.tsv`. Log lines, commands and research output stay in the code.

## The table

The file is tab-separated UTF-8. The first row names the columns: `key`, `en`,
then one column per further language (only English so far). Lines starting
with `#` are comments, and blank lines are skipped. Each other row is a key and
its text in each language:

```
key	en
fight.vote.of_set	of %d to win the set
fight.vote.delay_frame	%d frame
fight.vote.delay_frames	%d frames
```

- A key is lower case letters, digits, `_` and `.`, grouped by screen
  (`fight.vote.delay`). Each key appears once.
- In the text, `\n` is a line break, `\t` a tab and `\\` a backslash. Any
  other character, accented letters and symbols included, is written as it
  is.
- A cell left empty in another language shows the English.
- Text is a whole sentence or label. A sentence with a number or a name in it
  is one row with a printf conversion (`%d`, `%s`), never pieces joined in
  the code, so each language can order its own words. A count gets two rows,
  one for 1 and one for the rest (`delay_frame`, `delay_frames`).
- A percent sign in the text is written `%%`.

## The check

The build runs `polarity_strings` (`src/tools/strings_gen.cpp`) on the table
and stops with the file and line of any problem:

- the header row, an unknown escape, a key in the wrong form or given twice;
- a row with no English text;
- a `%` that is not a printf conversion;
- a translation whose printf conversions differ from the English ones, or
  come in another order.

It writes `strings_table.h` in the build folder: an `enum class Str` with a
member per key (`.` becomes `_`) and the text of every language. A key the
code names that the table lacks is therefore a compile error. `ctest -R strings`
runs the check on the tables in `tests/strings/`: the good one passes, and each
bad one has to fail.

## In the code

`core/text.h` gives `TR(Str::fight_vote_rematch)`, the text in the current
language, and `TRF(Str::fight_vote_of_set, 3)`, the text used as a printf
format. `TextSetLanguage` picks the language by its column (0 is English).

Adding a language is a column in the table. Text in the screens has to fit
in every language, so a new or longer string means a screenshot of each
window it appears in, at the narrowest size that window gets.
