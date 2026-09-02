# Computer Science Notes

## 1. Algorithms

> An algorithm is a finite sequence of well-defined instructions used to solve a problem.

A useful mental model is:

1. Define the problem.
2. Identify the input.
3. Determine the required output.
4. Design the algorithm.
5. Analyze its complexity.
6. Test the implementation.

### Big-O

For a simple loop:

```c
for (i = 0; i < n; i++) {
    printf("%d\n", i);
}
```

The loop executes `n` times, so its running time is approximately **O(n)**.

For a nested loop:

```c
for (i = 0; i < n; i++) {
    for (j = 0; j < n; j++) {
        work(i, j);
    }
}
```

The running time is approximately **O(n^2)**.

---

## 2. Data Structures

| Structure | Typical Access | Typical Search |
| --- | --- | --- |
| Array | O(1) | O(n) |
| Linked List | O(n) | O(n) |
| Hash Table | O(1) average | O(1) average |
| Binary Search Tree | O(log n) average | O(log n) average |

A useful rule:

> Choose the data structure based on the operations you need, not because the structure is fashionable.

### Definitions

Stack
: Last-in, first-out (LIFO) data structure.

Queue
: First-in, first-out (FIFO) data structure.

---

## 3. Operating Systems

The operating system manages resources such as:

- CPU time
- Memory
- Files
- Devices
- Processes

A process can be thought of as a program in execution.

```text
+------------------+
|    Application   |
+------------------+
         |
+------------------+
|  Operating Sys.  |
+------------------+
         |
+------------------+
|     Hardware     |
+------------------+
```

### A Tiny C Example

```c
#include <stdio.h>

int main(void)
{
    printf("Hello, world!\n");
    return 0;
}
```

The source code is compiled into an executable program.

---

## 4. Networking

The classic layered model is useful because each layer has a distinct responsibility.

1. Application
2. Transport
3. Network
4. Data Link
5. Physical

A packet travels through several layers before reaching its destination.

**Remember:** `127.0.0.1` refers to the local host.

---

## 5. Things to Review

- [x] Big-O notation
- [x] Arrays
- [x] Linked lists
- [ ] Hash table collision strategies
- [ ] TCP congestion control
- [ ] Virtual memory

A useful reminder from lecture:

> Premature optimization is often a distraction from understanding the problem.

[^csnote]: This is a deliberately simple footnote for testing Markdown footnotes.

The formula for the area of a circle is $A = \pi r^2$.

---

## 6. References

See the [Markdown Guide](https://www.markdownguide.org/) for Markdown syntax.

The following statement is intentionally ~~incorrect~~ corrected.

That is all for today's notes.
