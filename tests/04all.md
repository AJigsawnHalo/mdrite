# mdrite Markdown Feature Test

This document is a **general-purpose stress test** for `mdrite`. It intentionally contains every syntax element listed on the Markdown Guide cheat sheet.

## Basic Syntax

### Headings

There are multiple heading levels in this document.

#### Heading Level Four

##### Heading Level Five

###### Heading Level Six

### Text Styles

This is **bold**, this is *italic*, and this is `inline code`.

This sentence contains a [Markdown Guide link](https://www.markdownguide.org/).

### Blockquote

> The best debugging tool is a clear understanding of what the program is supposed to do.

### Lists

Unordered list:

- Alpha
- Beta
- Gamma

Ordered list:

1. First
2. Second
3. Third

### Horizontal Rule

---

### Image

![A placeholder landscape image](https://example.com/landscape.jpg)

## Extended Syntax

### Table

| Feature | Example | Status |
| --- | --- | --- |
| Heading | `# Title` | Test |
| Bold | `**text**` | Test |
| Italic | `*text*` | Test |
| Code | `` `code` `` | Test |
| Link | `[text](url)` | Test |

### Fenced Code Block

```c
#include <stdio.h>

int main(void)
{
    printf("Hello from mdrite!\n");
    return 0;
}
```

### Footnote

Here is a sentence with a footnote.[^1]

[^1]: This is the footnote text.

### Heading ID

### A Heading With a Custom ID {#custom-id}

This heading includes an explicit identifier.

### Definition List

Markdown
: A lightweight markup language.

mdrite
: A small text-mode Markdown editor for MS-DOS and FreeDOS.

### Strikethrough

~~This sentence should look crossed out.~~

### Task List

- [x] Write a test document
- [x] Open it in mdrite
- [ ] Check every feature
- [ ] Report anything broken

### Emoji

That was a great test! :joy:

### Highlight

I need to highlight these ==very important words==.

### Subscript

Water is H~2~O.

### Superscript

The area of a circle is proportional to r^2^.

## Mixed Test

> **Important:** Markdown features can be combined in the same document.

- **Bold list item**
- *Italic list item*
- `Code list item`
- ~~Struck list item~~
- ==Highlighted list item==

A paragraph can contain **bold**, *italic*, `code`, ~~strikethrough~~, ==highlight==, H~2~O, and x^2^ in one place.

---

## Final Checklist

- [x] Headings
- [x] Bold
- [x] Italic
- [x] Blockquote
- [x] Ordered list
- [x] Unordered list
- [x] Inline code
- [x] Horizontal rule
- [x] Link
- [x] Image
- [x] Table
- [x] Fenced code block
- [x] Footnote
- [x] Heading ID
- [x] Definition list
- [x] Strikethrough
- [x] Task list
- [x] Emoji
- [x] Highlight
- [x] Subscript
- [x] Superscript

**End of test.**
