# DockPin

Keeps the macOS Dock on the main display. It stops the Dock from jumping to
another monitor and brings it back if it ended up there anyway.

About 2 MB of memory, 0% CPU when idle, no icon and no UI.

## Install

```sh
brew tap indapublic/dockpin https://github.com/indapublic/dockpin
brew trust --formula indapublic/dockpin/dockpin   # Homebrew 7+ asks to trust third-party taps
brew install indapublic/dockpin/dockpin
brew services start dockpin
```

Then enable DockPin in System Settings > Privacy & Security > Accessibility.

## Update

```sh
brew upgrade dockpin
brew services restart dockpin
```

macOS asks for the Accessibility permission again after an update, because
the new build has a different signature. DockPin removes the stale entry
itself, so you only need to switch it on.

## Uninstall

```sh
brew services stop dockpin
brew uninstall dockpin
```

Then remove DockPin from System Settings > Privacy & Security > Accessibility
with the − button.

## How it works

The Dock moves to another display when the pointer pushes against that
display's Dock edge (the bottom one, or a side if the Dock is on the side).
DockPin drops mouse-move events inside a 4 pt band along that edge of every
non-main display. The cursor still moves normally, but the Dock never sees
the push. Parts of the edge that border another display are left alone, so
the cursor crosses over there as usual.

At startup and after display changes, a short-lived child process checks
where the Dock is and, if needed, brings it back to the main display by
briefly pushing the pointer against that display's edge.

The main display is the one marked as main in System Settings > Displays.

## Development

```sh
make        # builds DockPin.app
make test   # runs the edge-logic tests
```

To release: commit, tag `vX.Y.Z`, push the tag, then update `url` and
`sha256` in `Formula/dockpin.rb` and push the formula.

## License

MIT
