class Dockpin < Formula
  desc "Keeps the macOS Dock on the main display"
  homepage "https://github.com/indapublic/dockpin"
  url "https://github.com/indapublic/dockpin/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "cef8754c2f72c0b4bdd25784baa1315ce3759d8d0763ba1effa3e97e0fc71656"
  license "MIT"
  head "https://github.com/indapublic/dockpin.git", branch: "main"

  depends_on :macos

  def install
    system "make", "DockPin.app"
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
      After an upgrade macOS asks for this permission again.
    EOS
  end

  test do
    assert_predicate prefix/"DockPin.app/Contents/MacOS/DockPin", :executable?
    system "codesign", "--verify", prefix/"DockPin.app"
  end
end
