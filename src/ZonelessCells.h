#pragma once

namespace ZonelessCells
{
	// Enemies won't follow the player into a cell that has no encounter zone. While the player is in
	// such a cell, give it the no-boundary zone from RelentlessPursuit.esp (same approach as Enemies
	// Follow Me). It only lives in the cell's loaded data, so it is dropped when the cell unloads.
	void Install();

	// For the cell the player is in right after a save loads or a new game starts.
	void ApplyToPlayerCell();
}
