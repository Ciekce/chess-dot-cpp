#pragma once

#include "board.h"

class AttacksGenerator
{
public:
	static Bitboard GetAttackedByPawns(Bitboard myPawns, bool whiteToMove);
	static Bitboard GetAllAttacked(const Board& board, bool whiteToMove, Bitboard allPieces, Bitboard canAttackFrom = ~0ULL);
	static Bitboard GetAttackersOfSide(const Board& board, Position position, bool byWhite, Bitboard allPieces);
	static Bitboard GetCheckers(const Board& board);
};