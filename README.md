# pami-lisp

Minimal Lisp implementation to be used as a shell in microcontrollers.

The whole parsing and interpreting of code cannot use recursion, the whole
heap must fit whithin a delimited region set by the user. All code being
interpreted must be considered non-trusted code, and must be sandboxed
as much as possible.
