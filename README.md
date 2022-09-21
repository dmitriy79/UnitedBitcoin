UnitedBitcoin
=====================================

[![pipeline status](https://gitlab.com/bygage/UnitedBitcoin/badges/contractdev/pipeline.svg)](https://gitlab.com/bygage/UnitedBitcoin/commits/contractdev)


http://www.ub.com

What is UnitedBitcoin?
----------------
UnitedBitcoin is referred to as UB (official website: ub.com) whose currency unit is UBTC. The total volume, block-speed and halving time are exactly same as Bitcoin’s. Also, UnitedBitcoin’ s PoW mechanism is the same as Bitcoin, only initial difficulty is lowered. 

UnitedBitcoin supports SegWit and 8MB block-sizes to improve on-chain scalability, adds new smart contract features and plans to support the Lightning Network. 

Implementing Lightning Network and Smart Contract functionality on top of the Bitcoin’s UXTO model is very exciting and not easy to implement. Technically, QTUM has been able to accomplish that and create a large world-wide technical support community which can be used for reference.

UnitedBitcoin’ s smart contract system is a newly developed technology showcasing the capabilities of the development team. The number of teams around the world that can independently develop virtual machines for smart contracts is limited. 

Having said that, Bitcoin has the advantages of being globally recognized for their stable monetary system, and having the first mover advantage, a position which a fork cannot easily overturn. However, looking at its business model, UnitedBitcoin has a chance to make a difference.


License
-------

UnionBitcoin is released under the terms of the MIT license. See [COPYING](COPYING) for more
information or see https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is regularly built and tested, but is not guaranteed to be
completely stable. [Tags](https://github.com/bitcoin/bitcoin/tags) are created
regularly to indicate new official, stable release versions of Bitcoin Core.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md).

The developer [mailing list](https://lists.linuxfoundation.org/mailman/listinfo/bitcoin-dev)
should be used to discuss complicated or controversial changes before working
on a patch set.

Developer IRC can be found on Freenode at #bitcoin-core-dev.

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled in configure) with: `make check`. Further details on running
and extending unit tests can be found in [/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/test), written
in Python, that are run automatically on the build server.
These tests can be run (if the [test dependencies](/test) are installed) with: `test/functional/test_runner.py`

The Travis CI system makes sure that every pull request is built for Windows, Linux, and OS X, and that unit/sanity tests are run automatically.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.

Translations
------------

Changes to translations as well as new translations can be submitted to
[Bitcoin Core's Transifex page](https://www.transifex.com/projects/p/bitcoin/).

Translations are periodically pulled from Transifex and merged into the git repository. See the
[translation process](doc/translation_process.md) for details on how this works.

**Important**: We do not accept translation changes as GitHub pull requests because the next
pull from Transifex would automatically overwrite them again.

Translators should also subscribe to the [mailing list](https://groups.google.com/forum/#!forum/bitcoin-translators).
