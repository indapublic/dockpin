class Dockpin < Formula
  desc "Keeps the macOS Dock on the main display"
  homepage "https://github.com/indapublic/dockpin"
  url "https://github.com/indapublic/dockpin/releases/download/v1.0.2/DockPin-1.0.2.zip"
  sha256 "365e6c9ff9bff8f23080fa584e329975fd2361948a95b5e21c917643cb44aa39"
  license "MIT"
  head "https://github.com/indapublic/dockpin.git", branch: "main"

  depends_on :macos

  def install
    # Release archives carry DockPin.app signed with a Developer ID, so the
    # Accessibility permission survives updates. Source archives and HEAD
    # are built here and signed ad hoc.
    system "make", "DockPin.app" unless File.exist?("DockPin.app")
    prefix.install "DockPin.app"
  end

  service do
    run opt_prefix/"DockPin.app/Contents/MacOS/DockPin"
    keep_alive true
    process_type :interactive
    # The per-CPU nano allocator alone would double the footprint.
    environment_variables MallocNanoZone: "0"
    error_log_path var/"log/dockpin.log"
  end

  def caveats
    <<~EOS
      Start DockPin now and at every login:
        brew services start dockpin

      Then allow it in System Settings > Privacy & Security > Accessibility.
    EOS
  end

  test do
    assert_predicate prefix/"DockPin.app/Contents/MacOS/DockPin", :executable?
    system "codesign", "--verify", prefix/"DockPin.app"
  end
end
