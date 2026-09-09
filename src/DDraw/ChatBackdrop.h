#pragma once

struct _OFFSCREEN;

// Draws a solid black rectangle behind each visible engine chat line in the
// top-left of the game screen, to improve legibility for vision-impaired
// players. It is a no-op unless the ctrl-F2 "Chat Text Backdrop" option is
// enabled (default off).
namespace ChatBackdrop
{
	// Installs an inline hook on the engine's ChatMessageWithLogo (DrawChatText)
	// @ 0x00464060 so the backdrop is drawn immediately before the chat text on
	// every render path (normal gameplay, megamap overlay, victory screen).
	// Call once during DLL init.
	void Install();

	// Fills the backdrop for the current chat lines into the given render
	// context. Called by the installed hook; safe to call directly with a
	// valid OFFSCREEN.
	void Draw(_OFFSCREEN* offscreen);

	// The three below are for ChatLayout, which draws its own per-line backdrop
	// when it owns the chat draw (Draw() assumes the engine's single column).

	// True iff the ctrl-F2 "Chat Text Backdrop" option is on.
	bool BackdropEnabled();

	// Pixel width of one chat-ring line, measured the way it will be drawn.
	// lineHOverride > 0 measures against a ChatFont atlas at that height;
	// 0 measures the engine's native size (pre-ChatFontSize behaviour).
	int MeasureLineWidth(const unsigned char* ringEntry, int lineHOverride = 0);

	// One solid black box, clipped to the surface.
	void FillBehind(_OFFSCREEN* offscreen, int left, int top, int right, int bottom);
}
