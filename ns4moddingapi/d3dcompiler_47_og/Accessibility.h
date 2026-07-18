#ifndef ACCESSIBILITY_H
#define ACCESSIBILITY_H
#pragma once

#include <string>

namespace moddingApi
{
	// Screen-reader accessibility layer for blind players.
	// Speaks menu/UI text through Tolk (NVDA / JAWS / SAPI) and exposes a
	// toggleable debug log of every message-id -> decoded-text conversion so
	// that menu message ids can be discovered instead of guessed.
	class Accessibility
	{
	public:
		// Load Tolk and open the debug log. Safe to call once; later calls no-op.
		static void Initialize();
		// Unload Tolk and close the log.
		static void Shutdown();
		// Poll the controller (repeat-last, debug toggle, navigation gating).
		// Called once per framework tick.
		static void Update();

		// Called from the game's message-decode hook. messageId is the raw id
		// the game looked up (e.g. "gamemodeselect_018"); decodedText is the
		// human-readable result (e.g. "Story"). Either may be null.
		static void OnMessageDecoded(const char* messageId, const char* decodedText);

		// Speak an already-cleaned UTF-8 string. interrupt=true cuts off any
		// current speech (use for focus changes); false queues after it (hints).
		static void Speak(const std::string& text, bool interrupt = true);
		// Re-speak the last spoken announcement.
		static void RepeatLast();

		// When true, every newly-seen message id is written to the debug log.
		static bool DebugLogging;

	private:
		static bool _available;
		static bool _initialized;

		// Strip game markup (<icon .../>, <color ...>, ...) and trim.
		static std::string CleanText(const char* raw);
		// True if this message id is one we currently announce automatically.
		static bool ShouldAnnounce(const char* messageId);
		static void DebugLog(const std::string& id, const std::string& text);
	};
}

#endif
