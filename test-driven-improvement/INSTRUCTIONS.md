# Tuning the Width Optimizer

## Mandatory Ground Rules
    
There's a list of files here: `/tmp/list.txt`

Read the first line, and locate the specified file in
`/tmp/funcs1`. Move the file to a temporary location (don't copy it!
we want to remove files so that we don't work on the same file twice)
and also delete the first line from `list.txt`, so that the list stays
up to date.

Each file contains a single function in LLVM IR. You can find it
easily by looking for a line that beings with "define ".
    
Next, optimize the file using this LLVM pass that tries to reduce the
number of LLVM width-changing instructions in it. These are `sext`, `zext`,
and `trunc`.

Invoke the pass like this:
    
```
/home/regehr/llvm-project-regehr/build/bin/opt \
    -load-pass-plugin /home/regehr/llvm-width-optimization/build/lib/libWidthOpt.so \
    -passes='width-opt' -S input.ll -o output.ll
```

Now, I need to you inspect the optimized LLVM function closely. Your
goal is to think hard about it and try to remove any remaining
width-changing instructions, but *without changing the meaning of the
function*. You don't need to do this all by yourself, you can verify
your changes using `alive-tv` which can be found at
`/home/regehr/alive2-regehr/`.

Changes you are allowed to make include:
1. Remove a width-changing instruction.
2. Change the widths of instructions that are not the width-changing ones.
3. Change the widths of integer constants.
4. Update which SSA variables are in instruction operands.

You are explicitly NOT allowed to make other changes, such as removing
instructions other than `sext`, `zext`, and `trunc`. You are
explicitly NOT allowed to change one instruction to another.