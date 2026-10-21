# THCraft home screen

Select **Settings → Display → UI Theme → THCraft**. Only the home screen
changes; other screens keep the Lyra look.

## Layout

From top to bottom:

1. Header: name, clock (when the device has a time), battery.
2. Continue Reading: the most recently opened book with its cover, title,
   author, progress and chapter.
3. Recent Reads: the next three recently opened books.
4. Two widget slots, set in Display settings. When only one slot is on, it
   uses the full width.
5. Icon bar: Library, Files, Stats, Transfer, OPDS or Plugins, Settings. Plugins
   appears when plugins are enabled. OPDS appears when an OPDS server is added.

The layout adapts to the screen. When the screen is too short, the widget row is
hidden first, then the recent row. Turning both widget slots off makes the
covers larger, so new cover thumbnails are generated once.

Progress and chapter come from `/.crosspoint/recent.json`. The reader updates
them from the last page it showed when you leave a book or the device sleeps.
Entries from older firmware have no progress yet. Home reads it once from the
book's cache and stores it. The chapter appears after you next read that book.
When a book's cache is cleared, its progress is hidden until you open it again.

## Buttons

| Button | Action |
| --- | --- |
| Left / Right | Move within a row. At the end of a row, move to the next row. |
| Up / Down (side buttons) | Move to the previous or next row. |
| Confirm | Open the selected item. |
| Back | Resume the current book. |

On touch devices, tap an item.

## Widgets

Set **Home Widget (Left)** and **Home Widget (Right)** in Display settings:

| Widget | Shows | Opens |
| --- | --- | --- |
| Off | Nothing | — |
| Custom Image | An image from `/widgets` | Display settings |
| Today | Minutes and sessions today, reading time per hour | Reading Stats |
| 7 Days | Days read this week (Monday to Sunday) and the current streak | Reading Stats |
| Reading Goal | Minutes today against **Daily Reading Goal (min)** | Reading Stats |
| Clock | Time, weekday and date | Display settings |

### Custom image

Copy a JPEG, PNG, GIF or BMP file into `/widgets` on the SD card. Then set the
slot to **Custom Image** and choose the file in **Left/Right Widget Image**. The
list shows up to 32 files. Names of 47 bytes or longer are skipped.

JPEG, PNG and GIF images are converted once to a black-and-white dithered BMP
that covers the slot. The middle is kept and the edges are cropped. The result
is stored in `/.crosspoint/widgets/`.

The conversion is redone when the file size or the slot size changes. A
replacement file with exactly the same size keeps the old conversion; rename
the file to force a new one. When a conversion fails, home shows **Add an image
to /widgets** and does not retry until the device restarts or the file or slot
changes.

BMP files are not converted. They are scaled down to fit the slot.

## Reading statistics

Time on a page counts when you turn forward to the next page. Each page counts
for at least 2 seconds and at most 30 minutes, the same rule as the
`reader.session` [plugin event](plugin-events.md). Time on the page you leave the book from
is not counted.

A session counts when it has at least 10 seconds of reading. It is dated when
it ends. A day counts toward the streak once it has 60 seconds of reading.

The data is stored in `/.crosspoint/reading_stats.json`. It keeps the last 30
days, today's time per hour, streaks and the number of finished books. A book
counts as finished when the end-of-book screen appears. Each title and author
pair is counted once among the last 32 finished books.

Statistics are recorded only while the clock is confirmed for the current
power cycle. A Wi-Fi time sync confirms it. On boards with a battery-backed
clock chip (RTC), a valid RTC reading at boot also confirms it. Deep sleep keeps
the confirmation. After a power loss, a board without an RTC (such as the X4)
resumes from a saved time that can be behind. It records nothing until the next
Wi-Fi sync. Until then, the widgets and the Reading Stats screen say **Sync the
clock to track reading**. The finished-book total is always shown, because it
does not depend on the date.

Boards without an RTC keep time through deep sleep with an internal oscillator.
After a long sleep the clock can drift, so reading near midnight may be counted
on the wrong day until the next sync.
