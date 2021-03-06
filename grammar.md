## Shady language syntax

For the front-end mode, we can easily extend this by redefining `VALUE` as an arbitrary computation

```
PROGRAM := (DECL)*

DECL := const [TYPE] IDENTIFIER = VALUE; // constant definition
        fn IDENTIFIER Q_TYPES (PARAMS) BLOCK
        var ADDRESS_SPACE TYPE IDENTIFIER;

VAR := IDENTIFIER

VALUE := VAR | LITERAL
VALUES := VALUE+

PARAMS := ( [QTYPE IDENTIFIER [(, QTYPE IDENTIFIER)*]] )

CONTINUATION := cont PARAMS BLOCK

BLOCK := { (LET;)* TERMINATOR; [CONTINUATION*] } // the list of continuations is only for the front-end

CALLEE := (DECL | VALUE) // calls can be direct or indirect

LET := let IDENTIFIER [(, IDENTIFIER)*] = INSTRUCTION; // Some instructions have results
     | INSTRUCTION;                                    // some don't

INSTRUCTION := PRIMOP OPERANDS                    // primop
             | call (OPERAND) OPERANDS            // direct-style call
             | if Q_TYPES (OPERAND)               // structured if statement, can be defined to yield values
                   then BLOCK 
                   else (BLOCK | OPERANDS);       // else case can be a block, or default yield values
             | match Q_TYPES (OPERAND)            // structured match statement
                   (case LITERAL BLOCK)* 
                   default (BLOCK | OPERANDS);    // default case can be a block, or default yield values
             | loop Q_TYPES DEFAULT_PARAMS BLOCK  // structured loop statement

TERMINATOR := unreachable;                              // use as a placeholder if nothing belongs. undefined behaviour if reached.
            
            | jump (IDENTIFIER) OPERANDS;               // one-way non-divergent branch, target is immediate and must be uniform
            | branch (OPERAND, ID, ID) OPERANDS;        // two-way divergent branch, targets are immediate and must be uniform
            | switch (OPERAND)                          // n-way divergent branch, targets are immediate and must be uniform
                (case LITERAL ID)* 
                default (ID);
            | tailcall OPERAND OPERANDS;                // Start over in a new function, target is indirect (pointer), may be non-uniform
            
            | joinc ID OPERAND OPERANDS;                // yields to the innermost if/match statement
            | joinf OPERAND OPERAND OPERANDS;           // yields to the innermost if/match statement
            
            | return OPERANDS;                          // return from current function
            | callc OPERAND ID OPERANDS;                // call a function with a return continuation
            | callf OPERAND OPERAND OPERANDS;           // call a function with a return function
            
            | merge OPERANDS;                           // Merges the current structured if/match construct
            | continue OPERANDS;                        // Structured continue
            | break OPERANDS;                           // Structured break

TYPE := void | int | float | ptr DATA_TYPE | fn RET_TYPE ( [QTYPE [(, QTYPE)*]] )

DATA_TYPE := TYPE | struct { (TYPE IDENTIFIER;)* }

MQ_TYPES = MQ_TYPE [(, MQ_TYPE)*]
Q_TYPES = Q_TYPE [(, Q_TYPE)*]

VARIANCE_Q = uniform | varying

// qualified and maybe-qualified types
// maybe-qualified have inferrable unifornity
QTYPE = VARIANCE_Q TYPE
MQTYPE = [VARIANCE_Q] TYPE

```

## Control flow instructions

| Target   | Function call* | One-way jump         | One-way jump with sync |
|----------|----------------|----------------------|------------------------|
| direct   | callc          | br_jump / br_if_else | joinc                  |
| indirect | callf          | tailcall             | joinf                  |