#pragma once

// Retire queued callbacks before a level tears down its actor or HUD.
void cancel_deferred_alife_save();
