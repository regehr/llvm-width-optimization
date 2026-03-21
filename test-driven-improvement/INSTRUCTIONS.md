# Tuning the Width Optimizer

## Mandatory Ground Rules

This file describes a loop. You are to follow the instructions in it
repeatedly, until you reach one of the specified termination
conditions.

You are allowed and encouraged to automate steps by writing short
python scripts. For example, the first part of "Procedure" below
probably wants to be scripted. If you do this, put the scripts in this
same directory and update this document to specify using the tool
instead of performing the described steps manually. Every time you
do this, ask me first and provide a clear description of what you are
automating.
      
## Procedure
    
There's a list of files here: `/tmp/list.txt`. Read the first line of
this file, and locate the specified file in `/tmp/funcs1`. Move the
file to a temporary location (don't copy it! we want to remove files
so that we don't work on the same file twice) and also delete the
first line from `list.txt`, so that the list stays up to date.

The file you just moved will contain a single function in LLVM IR. You
can find it easily by looking for a line that begins with "define ".
    
Next, optimize the file using the LLVM pass from this repo, that tries
to reduce the number of LLVM width-changing instructions. These are
`sext`, `zext`, and `trunc`.

Invoke the pass like this:
    
```
/home/regehr/llvm-project-regehr/build/bin/opt \
    -load-pass-plugin /home/regehr/llvm-width-optimization/build/lib/libWidthOpt.so \
    -passes='width-opt' -S input.ll -o output.ll
```

If no width-changing instructions remain in the function, then you are
done with this function -- please proceed back to the start of the
"Procedure" part of this document.
    
Now, the hard part: I need to you inspect the optimized LLVM function
closely. Your goal is to think hard about it and try to remove any
remaining width-changing instructions, but *without changing the
meaning of the function*. You don't need to do this all by yourself,
you can verify your changes using `alive-tv --disable-undef-input`
which can be found at `/home/regehr/alive2-regehr/`. Since alive-tv is
a very powerful refinement checker, I encourage you to be brave: you
can try out different transformations even if you're not really sure
that they're correct.

Changes you are allowed to make while optimizing the function include:
1. Remove a width-changing instruction.
2. Change the widths of other instructions.
3. Change the widths of integer constants.
4. Update which SSA variables are in instruction operands.

You are explicitly NOT allowed to make other changes, such as removing
instructions other than `sext`, `zext`, and `trunc`. You are
explicitly NOT allowed to change one instruction to a different one.

If, after some effort, you are unable to remove any width-changing
instructions without changing the meaning of the function, then you
should loop: go back to the start of this "Procedure" section and try
a different function. Do this autonomously, no need to stop.
    
You should come back to me with a prompt any time you find that you
can width-optimize a function yourself, but our pass was unable to do
so. Present me with that function as the last thing you do before
giving me a prompt back.

