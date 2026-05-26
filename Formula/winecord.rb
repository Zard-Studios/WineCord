class Winecord < Formula
  desc "Discord Rich Presence bridge for Wine/CrossOver games on macOS"
  homepage "https://github.com/Zard-Studios/WineCord"
  url "https://raw.githubusercontent.com/Zard-Studios/homebrew-tap/main/releases/winecord-0.1.4-macos-universal.tar.gz"
  sha256 "62fa5d9e523d5f6da59a5ebfc986af05a9bb5ad6a4f484829857aaddac1b0b49"
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
