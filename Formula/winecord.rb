class Winecord < Formula
  desc "Discord Rich Presence bridge for Wine/CrossOver games on macOS"
  homepage "https://github.com/Zard-Studios/WineCord"
  url "https://github.com/Zard-Studios/WineCord/releases/download/v0.1.0/winecord-0.1.0-macos-universal.tar.gz"
  sha256 "fec88c9504fed98e613ccb5d01cde701d0d2763d77290b9731c2069c4a43d0c8"
  license "MIT"

  def install
    bin.install "bin/winecord"
    libexec.install "libexec/winecord"
    doc.install "README.md"
    prefix.install "LICENSE", "NOTICE"
  end

  service do
    run [opt_bin/"winecord", "agent"]
    run_type :immediate
    keep_alive false
    log_path var/"log/winecord/agent.log"
    error_log_path var/"log/winecord/agent.err.log"
  end

  def caveats
    <<~EOS
      Finish setup with:
        winecord setup

      Remove WineCord's LaunchAgent and CrossOver bottle helper before uninstalling:
        winecord uninstall
        brew uninstall winecord

      If your CrossOver Steam bottle is not auto-detected:
        winecord setup --bottle /path/to/CrossOver/Bottles/Steam
    EOS
  end

  test do
    assert_match "WineCord", shell_output("#{bin}/winecord --version")
  end
end
