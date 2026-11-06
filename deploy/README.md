# deploy

Updates an installed card from the Mac, without sudo.

```sh
bazel run //deploy:sd -- disk4                                  # dry run: what would change
bazel run //deploy:sd -- disk4 --yes --confirm=disk4            # deploy
bazel run //deploy:sd --//deploy:logs -- disk4 --yes --confirm=disk4      # and pull the card's logs first
bazel run //deploy:sd --//deploy:harvest -- disk4 --yes --confirm=disk4   # and merge the device's m2v journals
bazel run //deploy:pull_logs -- disk4                           # logs only
```

A deploy writes only what differs from the card. The boot entry and overlay are compared file by file. The GPU bundle,
shader seed and raw pack go through the stager, which keeps a list of what it staged in `<Data>/ngpu/stage.tsv.gz`. When
the build matches that list the stager reads nothing else, so a deploy after a driver change writes the boot entry and
ejects. The seed ships as a few hundred pack files because every file created over hekate's USB mass storage costs more
than its bytes, and the old one-file-per-entry seed had 112k of them.

Logs and harvesting are off by default: live logs over USB (`idevicesyslog archive`) cover the usual case, and the
device keeps its own shader cache. `--pull-logs` and `--harvest` (or `--no-...`) on the command line override the flags.
Pulled logs land in `~/Library/Application Support/switch-ios/device-logs/mirror`, which copies only new or grown files,
and each pull's `boot.logarchive` is made of hard links into it.

`--//deploy:gpu_bundle` defaults to `//gpu:bundle`, a manifest over Bazel's outputs that nothing copies on the Mac.
Point it at an lkg snapshot's `gpu/` directory to stage that build instead. `--remove-gpu` takes the bundle off the
card.
