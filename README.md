# Problem 016 - File Storage BPT

**ACMOJ Problem ID**: 2697

## Table of Contents

- [Problem 016 - File Storage BPT](#problem-016---file-storage-bpt)
  - [Table of Contents](#table-of-contents)
  - [Problem Description](#problem-description)
  - [Input Format](#input-format)
  - [Output Format](#output-format)
  - [Sample](#sample)
    - [Input](#input)
    - [Output](#output)
  - [Data Range](#data-range)
  - [Per-Testcase Resource Limits](#per-testcase-resource-limits)
  - [Submission Requirements](#submission-requirements)
    - [OJ Git Repository Compilation Process](#oj-git-repository-compilation-process)
    - [Git Configuration Requirements](#git-configuration-requirements)
    - [Submission Guidelines](#submission-guidelines)
    - [Evaluation Notes](#evaluation-notes)
    - [Academic Integrity](#academic-integrity)

## Problem Description

You need to implement a key-value database based on file storage, which provides similar functionality to `std::map` on files.

The implementation must use BPT (B+ Tree).

This problem has three types of input commands:

* `insert [index] [value]`
  * Insert an entry with index `[index]` and value `[value]`

* `delete [index] [value]`
  * Delete an entry with index `[index]` and value `[value]`. **Note: the entry to be deleted may not exist**

* `find [index]`
  * Find all entries with index `[index]` and output their `[value]` in **ascending order**, separated by spaces. If no entry with index `[index]` exists, output `null`. Output a newline after the entire command output

Where `[index]` is a string of no more than 64 bytes (no whitespace characters), and `[value]` is an integer within the `int` range.

In the data, both `[index]` and `[value]` may be duplicated, but the same `[index]` cannot correspond to duplicate `[value]`s.

There are no public test cases except for the sample.

## Input Format

The first line contains an integer n, representing the total number of commands.

The next n lines contain n commands, each being one of the three command types mentioned above.

## Output Format

After executing a `find index` command, find all entries with index `index` and output their `value` in ascending order, separated by spaces. If no entry with index `index` exists, output `null`. Output a newline after the entire command output.

## Sample

### Input

```
8
insert FlowersForAlgernon 1966
insert CppPrimer 2012
insert Dune 2021
insert CppPrimer 2001
find CppPrimer
find Java
delete Dune 2021
find Dune
```

### Output

```
2001 2012
null
null
```

## Data Range

- $1\le n \le 300000$
- Total number of stored entries does not exceed $300000$
- Input data is guaranteed to be valid

**Note: Some test cases require continuing operations based on previous run results. File cleanup will be handled by the judge, so you only need to check if the file exists and create one if it doesn't.**

## Per-Testcase Resource Limits

- **Time Limit (per test case)**: 500 ms (min), 5000 ms (max)
- **Memory Limit (per test case)**: 64 MiB
- **Disk Space Limit**: 1024 MiB
- **File Count Limit**: 20 files

## Submission Requirements

### OJ Git Repository Compilation Process

For Git compilation, we will first clone the repository using a command similar to:
```bash
git clone <repo_url> . --depth 1 --recurse-submodules --shallow-submodules --no-local
```

Then we check if there is a `CMakeLists.txt` file. If it exists, we run (if not, a warning message will be displayed):
```bash
cmake .
```

Finally, we check if there is any of `GNUmakefile`/`makefile`/`Makefile` (if cmake was run previously, this will be the generated Makefile). If it exists, we run (if not, a warning message will be displayed):
```bash
make
```

After this process is complete, we will use the `code` file in the project root directory as the compilation result.

The project does not provide a CMakeLists.txt file, so you need to create and edit it yourself. The local environment has gcc-13 and g++-13 available.

### Git Configuration Requirements

**IMPORTANT**: You must create a `.gitignore` file in your project root directory to avoid OJ evaluation conflicts.

The `.gitignore` file should include at least the following entries:

```gitignore
CMakeFiles/
CMakeCache.txt
```

### Submission Guidelines

- The submitted code must be able to compile successfully through the above compilation process
- The compiled executable file name must be `code`
- The program needs to be able to read data from standard input and write results to standard output
- Please ensure the code runs correctly within the given time and space limits
- **You must use C++ or C language** to implement this assignment

### Evaluation Notes

- The evaluation system will test your program using the provided test data
- The program output must exactly match the expected output (including format)
- Exceeding time or memory limits will be judged as the corresponding error type
- Please pay attention to the overall time performance of your code and the time complexity of each part of your algorithm

### Academic Integrity

If any violations are found during evaluation or code review (including but not limited to using unconventional methods to pass test cases), your final score may be significantly reduced or become **0 points**.

