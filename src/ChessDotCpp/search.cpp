#include "search.h"


#include "attacks.h"
#include "movegen.h"
#include "evaluation.h"
#include "moveorder.h"
#include "see.h"
#include "stopper.h"

//bool Search::TryProbeTranspositionTable(const ZobristKey key, const Ply depth, const Score alpha, const Score beta, Move& bestMove, Score& score, bool& entryExists)
//{
//    score = 0;
//    entryExists = false;
//    TranspositionTableEntry entry;
//    ZobristKey entryKey;
//    
//    const bool found = State.Global.Table.TryProbe(key, &entry, &entryKey);
//    if (!found)
//    {
//        State.Stats.HashMiss++;
//        return false;
//    }
//
//    if (entryKey != key)
//    {
//        State.Stats.HashCollision++;
//        return false;
//    }
//
//    entryExists = true;
//    bestMove = entry.MMove;
//
//    if (entry.GetDepth() < depth)
//    {
//        State.Stats.HashInsufficientDepth++;
//        return false;
//    }
//
//    switch (entry.GetFlag())
//    {
//    case TranspositionTableFlags::Exact:
//        score = entry.GetScore();
//        return true;
//        
//    case TranspositionTableFlags::Alpha:
//        if (entry.GetScore() <= alpha)
//        {
//            score = alpha;
//            return true;
//        }
//        return false;
//        
//    case TranspositionTableFlags::Beta:
//        if (entry.GetScore() >= beta)
//        {
//            score = beta;
//            return true;
//        }
//        return false;
//        
//    default:
//        assert(false);
//        break;
//    }
//
//    return false;
//}

bool Search::TryProbeTranspositionTable(const ZobristKey key, const Ply depth, const Score alpha, const Score beta, TranspositionTableEntry& entry, Score& score, bool& entryExists)
{
    score = 0;
    entryExists = false;
    ZobristKey entryKey;

    const bool found = State.Global.Table.TryProbe(key, &entry, &entryKey);
    if (!found)
    {
        State.Stats.HashMiss++;
        return false;
    }

    if (entryKey != key)
    {
        State.Stats.HashCollision++;
        return false;
    }

    entryExists = true;

    if (entry.Depth < depth)
    {
        State.Stats.HashInsufficientDepth++;
        return false;
    }

    switch (entry.Flag)
    {
    case TranspositionTableFlags::Exact:
        score = entry.SScore;
        return true;

    case TranspositionTableFlags::Alpha:
        if (entry.SScore <= alpha)
        {
            score = alpha;
            return true;
        }
        return false;

    case TranspositionTableFlags::Beta:
        if (entry.SScore >= beta)
        {
            score = beta;
            return true;
        }
        return false;

    default:
        assert(false);
        break;
    }

    return false;
}

void Search::StoreTranspositionTable(const ZobristKey key, const Move move, const Ply depth, const Score score, const TtFlag flag)
{
    if (Stopper.Stopped)
    {
        return;
    }
    
    State.Global.Table.Store(key, move, depth, score, flag);
}

Score Search::Contempt(const Board& board) const
{
    (void)board;
    return 0;
}

bool Search::IsRepetitionOr50Move(const Board& board) const
{
    if (board.HistoryDepth - board.FiftyMoveRuleIndex > 100)
    {
        return true;
    }

    for (HistoryPly ply = board.FiftyMoveRuleIndex; ply < board.HistoryDepth; ply++)
    {
        const auto& previousEntry = board.History[ply];
        const ZobristKey previousKey = previousEntry.Key;
        if (board.Key == previousKey)
        {
            return true;
        }
    }
    return false;
}

Score Search::Quiescence(Board& board, Ply depth, Ply ply, Score alpha, Score beta)
{
    ++State.Stats.Nodes;
    
    constexpr int threadId = 0;
    const bool rootNode = ply == 0;
    
    EachColor<Bitboard> pins;
    PinDetector::GetPinnedToKings(board, pins);
    const Score standPat = Evaluation::Evaluate(board, pins, State.Global.Eval);

    if (standPat >= beta)
    {
        return beta;
    }

    if (alpha < standPat)
    {
        alpha = standPat;
    }

    const Bitboard checkers = AttacksGenerator::GetCheckers(board);
    const bool inCheck = checkers != BitboardConstants::Empty;

    const Bitboard pinned = pins[board.ColorToMove];

    MoveArray moves;
    MoveCount moveCount = 0;
    MoveGenerator::GetAllPotentialCaptures(board, checkers, pinned, moves, moveCount);

    const Move previousMove = rootNode ? Move(0) : board.History[board.HistoryDepth - 1].Move;
    const Move countermove = State.Thread[threadId].Countermoves[previousMove.GetPiece()][previousMove.GetTo()];

    ScoreArray seeScores;
    See::CalculateSeeScores(board, moves, moveCount, seeScores);
    
    MoveScoreArray staticMoveScores{};
    MoveOrdering::CalculateStaticScores(State, moves, seeScores, moveCount, ply, Move(0), countermove, staticMoveScores);

    Score bestScore = -Constants::Inf;
    Move bestMove;
    bool raisedAlpha = false;
    bool betaCutoff = false;
    uint8_t movesEvaluated = 0;
    for (MoveCount moveIndex = 0; moveIndex < moveCount; moveIndex++)
    {
        MoveOrdering::OrderNextMove(State, ply, moveIndex, moves, seeScores, staticMoveScores, moveCount);
        const Move move = moves[moveIndex];
        
        const bool valid = MoveValidator::IsKingSafeAfterMove2(board, move, checkers, pinned);
        if (!valid)
        {
            continue;
        }

        const Score takesMaterial = EvaluationConstants::PieceValues[move.GetTakesPiece()];
        const Score opponentMaterial = board.PieceMaterial[board.ColorToMove ^ 1];
        const Score resultMaterial = opponentMaterial - takesMaterial;
        
        // DELTA PRUNING
        if
        (
            !inCheck
            && standPat + takesMaterial + 200 < alpha
            && resultMaterial > Constants::EndgameMaterial
            && move.GetPawnPromoteTo() == Pieces::Empty
        )
        {
            continue;
        }

        // SEE PRUNING
        if
        (
            !inCheck
            && move.GetPawnPromoteTo() == Pieces::Empty
            &&
            (
                resultMaterial > Constants::EndgameMaterial
                || move.GetTakesPiece() == Pieces::WhitePawn
                || move.GetTakesPiece() == Pieces::BlackPawn
            )
        )
        {
            const Score seeScore = seeScores[moveIndex];
            if (seeScore < 0) // TODO: -10?
            {
                continue;
            }
        }

        board.DoMove(move);
        const Score childScore = -Quiescence(board, depth - 1, ply + 1, -beta, -alpha);
        board.UndoMove();
        movesEvaluated++;

        if (childScore > bestScore)
        {
            bestScore = childScore;
            bestMove = move;

            if (childScore > alpha)
            {
                alpha = childScore;
                raisedAlpha = true;

                if (childScore >= beta)
                {
                    betaCutoff = true;
                    break;
                }
            }
        }
    }

    if (betaCutoff)
    {
        return beta;
    }

    return alpha;
}

void UpdateHistory(MoveScore& score, const MoveScore value)
{
    const MoveScore absValue = std::abs(value);
    if(absValue > 324)
    {
        return;
    }

    score -= (score * absValue) / 324;
    score += value * 32;
}

Score Search::AlphaBeta(Board& board, Ply depth, const Ply ply, Score alpha, Score beta, bool isPrincipalVariation, bool nullMoveAllowed)
{
    constexpr int threadId = 0;
    ThreadState& threadState = State.Thread[threadId];
    PlyData& plyState = threadState.Plies[ply];
    const bool rootNode = ply == 0;
    
    if (depth > 2 && Stopper.ShouldStop())
    {
        const Score score = Contempt(board);
        return score;
    }

    // REPETITION DETECTION
    if (nullMoveAllowed && !rootNode)
    {
        const bool isDraw = IsRepetitionOr50Move(board);
        if (isDraw)
        {
            const auto score = Contempt(board);
            return score;
        }
    }

    // IN CHECK EXTENSION
    const Bitboard checkers = AttacksGenerator::GetCheckers(board);
    const bool inCheck = checkers != BitboardConstants::Empty;
    if (inCheck)
    {
        depth++;
    }
    
    if(depth <= 0)
    {
        //EachColor<Bitboard> pins;
        //PinDetector::GetPinnedToKings(board, pins);
        //const Score eval = Evaluation::Evaluate(board, pins);
        const Score eval = Quiescence(board, depth, ply, alpha, beta);
        return eval;
    }

    ++State.Stats.Nodes;
    MoveScore bonus = depth * depth + depth - 1;

    // PROBE TRANSPOSITION TABLE
    TranspositionTableEntry entry;
    bool hashEntryExists = true;
    Score probedScore;
    const bool probeSuccess = TryProbeTranspositionTable(board.Key, depth, alpha, beta, entry, probedScore, hashEntryExists);
    const Move principalVariationMove = hashEntryExists ? entry.MMove : Move(0);
    if (probeSuccess)
    {
        if (!isPrincipalVariation || (probedScore > alpha && probedScore < beta))
        {
            if (probedScore > Constants::MateThreshold)
            {
                probedScore -= ply;
            }
            else if (probedScore < -Constants::MateThreshold)
            {
                probedScore += ply;
            }

            if (principalVariationMove.GetTakesPiece() == Pieces::Empty)
            {
                UpdateHistory(threadState.History[principalVariationMove.GetColorToMove()][principalVariationMove.GetFrom()][principalVariationMove.GetTo()], bonus);
            }
            else
            {
                UpdateHistory(threadState.CaptureHistory[principalVariationMove.GetPiece()][principalVariationMove.GetTo()][principalVariationMove.GetTakesPiece()], bonus);
            }
            
            return probedScore;
        }
    }
    
    const Score currentMateScore = Constants::Mate - ply;

    // STATIC EVALUATION PRUNING
    EachColor<Bitboard> pins;
    PinDetector::GetPinnedToKings(board, pins);
    const Score staticScore = Evaluation::Evaluate(board, pins, State.Global.Eval);
    if
    (
        depth < 3
        && !isPrincipalVariation
        && !inCheck
    )
    {
        if (!(std::abs(beta - 1) > -Constants::Mate + 100))
        {
            Throw();
        }

        const Score margin = 120 * depth; // 120? Pawn is 100
        if (staticScore - margin >= beta)
        {
            return staticScore - margin;
        }
    }
    
    // NULL MOVE PRUNING
    if
    (
        nullMoveAllowed
        && !inCheck
        && depth > 2
        && !rootNode
    )
    {
        const Score material = board.PieceMaterial[board.ColorToMove];
        if (material > Constants::EndgameMaterial)
        {
            const Ply nullDepthReduction = depth > 6 ? 3 : 2;
            const Move nullMove = Move(0, 0, Pieces::Empty);
            board.DoMove(nullMove);
            const Score nullMoveScore = -AlphaBeta(board, depth - nullDepthReduction - 1, ply + 1, -beta, -beta + 1, false, false);
            board.UndoMove();
            if (nullMoveScore >= beta)
            {
                return beta;
            }
        }
    }

    // FUTILITY PRUNING - DETECTION
    bool futilityPruning = false;
    std::array<Score, 4> futilityMargins { 0, 200, 300, 500 };
    if
    (
        depth <= 3
        && !isPrincipalVariation
        && !inCheck
        && std::abs(alpha) < 9000
        && staticScore + futilityMargins[depth] <= alpha
    )
    {
        futilityPruning = true;
    }

    const bool improving = board.HistoryDepth < 2 || staticScore >= board.History[board.HistoryDepth - 2].StaticEvaluation;
    
    const Bitboard pinned = pins[board.ColorToMove];
    
    MoveArray moves;
    MoveCount moveCount = 0;
    MoveGenerator::GetAllPotentialMoves(board, checkers, pinned, moves, moveCount);

    const Move previousMove = rootNode ? Move(0) : board.History[board.HistoryDepth - 1].Move;
    const Move countermove = threadState.Countermoves[previousMove.GetPiece()][previousMove.GetTo()];

    ScoreArray seeScores;
    See::CalculateSeeScores(board, moves, moveCount, seeScores);
    
    MoveScoreArray staticMoveScores;
    MoveOrdering::CalculateStaticScores(State, moves, seeScores, moveCount, ply, principalVariationMove, countermove, staticMoveScores);

    Score bestScore = -Constants::Inf;
    Move bestMove;
    bool raisedAlpha = false;
    bool betaCutoff = false;
    uint8_t movesEvaluated = 0;

    MoveArray failedMoves;
    MoveCount failedMoveCount = 0;
    for (MoveCount moveIndex = 0; moveIndex < moveCount; moveIndex++)
    {
        MoveOrdering::OrderNextMove(State, ply, moveIndex, moves, seeScores, staticMoveScores, moveCount);
        const Move move = moves[moveIndex];

        /*if(move.Value == threadState.SingularMove.Value)
        {
            continue;
        }*/
        
        const bool valid = MoveValidator::IsKingSafeAfterMove2(board, move, checkers, pinned);
        if(!valid)
        {
            continue;
        }

        Ply extension = 0;
        //if
        //(
        //    !rootNode
        //    && depth >= 6
        //    //&& nullMoveAllowed
        //    && threadState.SingularMove.Value == 0
        //    && move.Value == principalVariationMove.Value
        //    && entry.Flag == TranspositionTableFlags::Beta
        //    && entry.Depth >= depth - 3
        //    && std::abs(entry.SScore) < Constants::MateThreshold
        //)
        //{
        //    const Score singularBeta = entry.SScore - 10 * depth;
        //    const Score singularAlpha = singularBeta - 1;
        //    const Ply singularDepth = depth / 2;
        //    
        //    threadState.SingularMove = move;
        //    const Score singularScore = AlphaBeta(board, singularDepth, ply, singularAlpha, singularBeta, false, true);
        //    threadState.SingularMove = Move(0);

        //    if(singularScore < singularBeta)
        //    {
        //        extension++;
        //    }
        //}

        const Score seeScore = seeScores[moveIndex];
        board.DoMove(move);

        // FUTILITY PRUNING
        if
        (
            futilityPruning
            && movesEvaluated > 0
            && move.GetTakesPiece() == Pieces::Empty
            && move.GetPawnPromoteTo() == Pieces::Empty
        )
        {
            const Position opponentKingPos = board.KingPositions[board.ColorToMove];
            const bool opponentInCheck = AttacksGenerator::IsPositionAttacked(board, opponentKingPos, !board.WhiteToMove);
            if (!opponentInCheck)
            {
                board.UndoMove();
                continue;
            }
        }

        // LATE MOVE REDUCTION
        Ply reduction = 0;
        if
        (
            movesEvaluated > 1
            && (!rootNode || movesEvaluated > 3)
            && depth >= 3
            && !inCheck
            && move.Value != plyState.Killers[0].Value
            && move.Value != plyState.Killers[1].Value
            //&& move.Value != countermove.Value
            && seeScore <= 0
            && move.GetPawnPromoteTo() == Pieces::Empty
        )
        {
            const auto pvReductionIndex = isPrincipalVariation ? 1 : 0;
            reduction = SearchData.Reductions[pvReductionIndex][depth][movesEvaluated];

            if(!isPrincipalVariation && !improving && reduction > 1)
            {
                reduction++;
            }

            if
            (
                reduction > 0
                &&
                (
                    threadState.History[move.GetColorToMove()][move.GetFrom()][move.GetTo()] > 0
                    || threadState.CaptureHistory[move.GetPiece()][move.GetTo()][move.GetTakesPiece()] > 0
                )
            )
            {
                reduction--;
            }


            if
            (
                !isPrincipalVariation
                && threadState.History[move.GetColorToMove()][move.GetFrom()][move.GetTo()] < 0
            )
            {
                reduction++;
            }
        }
        
        Score childScore;
        if(raisedAlpha)
        {
            childScore = -AlphaBeta(board, depth + extension- reduction - 1, ply + 1, -alpha - 1, -alpha, false, true);
            if(childScore > alpha)
            {
                childScore = -AlphaBeta(board, depth + extension - reduction - 1, ply + 1, -beta, -alpha, isPrincipalVariation, true);
            }
        }
        else
        {
            childScore = -AlphaBeta(board, depth + extension - reduction - 1, ply + 1, -beta, -alpha, isPrincipalVariation, true);
        }

        if (reduction > 0 && childScore > alpha)
        {
            childScore = -AlphaBeta(board, depth + extension - 1, ply + 1, -beta, -alpha, isPrincipalVariation, true);
        }
        
        board.UndoMove();
        movesEvaluated++;
        
        if(childScore > bestScore)
        {
            bestScore = childScore;
            bestMove = move;

            if(childScore > alpha)
            {
                alpha = childScore;
                raisedAlpha = true;

                if(rootNode)
                {
                    threadState.BestMoveChanges++;
                }
                
                if(childScore >= beta)
                {
                    betaCutoff = true;
                    break;
                }
            }
        }
        else
        {
            failedMoves[failedMoveCount++] = move;
        }
    }

    if (raisedAlpha)
    {
        for(MoveCount failedMoveIndex = 0; failedMoveIndex < failedMoveCount; failedMoveIndex++)
        {
            const Move failedMove = failedMoves[failedMoveIndex];
            if(failedMove.GetTakesPiece() == Pieces::Empty)
            {
                UpdateHistory(threadState.History[failedMove.GetColorToMove()][failedMove.GetFrom()][failedMove.GetTo()], -bonus);
            }
            else
            {
                UpdateHistory(threadState.CaptureHistory[failedMove.GetPiece()][failedMove.GetTo()][failedMove.GetTakesPiece()], -bonus);
            }
        }
        
        if(bestMove.GetTakesPiece() == Pieces::Empty)
        {
            UpdateHistory(threadState.History[bestMove.GetColorToMove()][bestMove.GetFrom()][bestMove.GetTo()], bonus);
        }
        else
        {
            UpdateHistory(threadState.CaptureHistory[bestMove.GetPiece()][bestMove.GetTo()][bestMove.GetTakesPiece()], bonus);
        }
        
        if (betaCutoff)
        {
            if (bestMove.GetTakesPiece() == Pieces::Empty)
            {
                plyState.Killers[1] = plyState.Killers[0];
                plyState.Killers[0] = bestMove;
                threadState.Countermoves[previousMove.GetPiece()][previousMove.GetTo()] = bestMove;
            }

            //if (threadState.SingularMove.Value == 0)
            {
                StoreTranspositionTable(board.Key, bestMove, depth, bestScore, TranspositionTableFlags::Beta);
            }
            
            return beta;
        }
    }

    // MATE / STALEMATE
    if(movesEvaluated == 0)
    {
        if(inCheck)
        {
            return -currentMateScore;
        }
        return Contempt(board);
    }

    //if (threadState.SingularMove.Value == 0)
    {
        if (raisedAlpha)
        {
            StoreTranspositionTable(board.Key, bestMove, depth, bestScore, TranspositionTableFlags::Exact);
        }
        else
        {
            StoreTranspositionTable(board.Key, bestMove, depth, bestScore, TranspositionTableFlags::Alpha);
        }
    }
    
    return alpha;
}

Score Search::Aspiration(Board& board, const Ply depth, const Score previous)
{
    //constexpr Score window = 50;
    //const Score alpha = previous - window;
    //const Score beta = previous + window;
    //const Score windowScore = AlphaBeta(board, depth, 0, alpha, beta, true, true);
    //if (windowScore > alpha && windowScore < beta)
    //{
    //    return windowScore;
    //}

    (void)previous;
    State.Thread[0].BestMoveChanges = 0;
    const Score fullSearchScore = AlphaBeta(board, depth, 0, -Constants::Inf, Constants::Inf, true, true);
    return fullSearchScore;
}

Move Search::IterativeDeepen(Board& board)
{
    Score score = AlphaBeta(board, 1, 0, -Constants::Inf, Constants::Inf, true, true);
    State.Global.Table.SavePrincipalVariation(board);
    State.Stats.Elapsed = Stopper.GetElapsed();
    SearchCallbackData callbackData(board, State, 1, score);
    
    Callback(callbackData);

    if (Stopper.ShouldStopDepthIncrease(State))
    {
        return State.Global.Table.SavedPrincipalVariation[0];
    }
    
    for (Ply depth = 2; depth < Constants::MaxDepth; depth++)
    {
        score = Aspiration(board, depth, score);
        callbackData.Depth = depth;
        callbackData._Score = score;
        const bool pvMoveChanged = State.Global.Table.IsRootMoveChanged(board);
    	if(pvMoveChanged)
    	{
            State.Thread[0].IterationsSincePvChange = 0;
    	}
        else
        {
            State.Thread[0].IterationsSincePvChange++;
        }
        State.Stats.Elapsed = Stopper.GetElapsed();
        if(Stopper.ShouldStopDepthIncrease(State))
        {
            break;
        }

        State.Global.Table.SavePrincipalVariation(board);
        Callback(callbackData);
    }

    return State.Global.Table.SavedPrincipalVariation[0];
}

Move Search::Run(Board& board, const SearchParameters& parameters)
{
    Stopper.Init(parameters, board.WhiteToMove);
    State.NewSearch();
    const auto move = IterativeDeepen(board);
    return move;
}
