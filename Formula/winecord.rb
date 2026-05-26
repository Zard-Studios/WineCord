class Winecord < Formula
  desc "Discord Rich Presence bridge for Wine/CrossOver games on macOS"
  homepage "https://github.com/Zard-Studios/WineCord"
  url "https://raw.githubusercontent.com/Zard-Studios/homebrew-tap/main/releases/winecord-0.1.5-macos-universal.tar.gz"
  sha256 "f9e378fe11a6ac56269fd31c8d5b52b39ee7dd28033934c3a14574cc853ceaf6"
  license "MIT"

  def install
    bin.install "bin/winecord"
    libexec.install "libexec/winecord"
    doc.install "README.md"
    prefix.install "LICENSE", "NOTICE"
  end

  def caveats
    <<~EOS
      Finish setup with:
        winecord setup

      Remove WineCord's LaunchAgent and CrossOver bottle helper before uninstalling:
        winecord uninstall
        brew uninstall winecord

      If the configured CrossOver bottle is on an external volume, connect it before running winecord uninstall.

      If your CrossOver Steam bottle is not auto-detected:
        winecord setup --bottle /path/to/CrossOver/Bottles/Steam
    EOS
  end

  test do
    assert_match "WineCord", shell_output("#{bin}/winecord --version")
  end
end
