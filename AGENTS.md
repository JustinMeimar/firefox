
- never write new tests unless asked.

- always understand and distill the resident spidermonkey implementation style when
  proposing code.

- abstain from writing pointless, superflousous comments. Pick variable and function names
  that adequately describes the semantics being implemented.

- good code accounts for every error case, handling each gracefully; great code makes
  invalid states unrepresentable. This is related to the principle of: "Parse, don't validate".
  If we can create types that by construction can not represent and invalid state, and parse
  in order to produce an object of the type, the code becomes much more readable.

- prefer strategic programming over defensive programming. (a la John Ousterhouts a
  philosophy of Software Design)

- prefer to assert rather than throw or log error messages, unless fallible behaviour is
  tolerated in the design.

