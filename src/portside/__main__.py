"""Allow ``python -m portside`` as well as the ``portside`` console script."""

from portside.cli.main import main

raise SystemExit(main())
