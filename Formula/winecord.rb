class Winecord < Formula
  desc "Discord Rich Presence bridge for Wine/CrossOver games on macOS"
  homepage "https://github.com/Zard-Studios/WineCord"
  url "https://github.com/Zard-Studios/WineCord/releases/download/v0.1.0/winecord-0.1.0-macos-universal.tar.gz"
  sha256 "06f980af72a0cf16e7e9431ab27851538add5064d5ac674151339a495773a5ad"
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
