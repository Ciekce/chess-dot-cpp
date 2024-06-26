﻿#include "ChessDotCpp.h"

#include "bench.h"
#include "tests.h"
#include "uci.h"
#include "evaluation.h"
#include "tablebases.h"
#include "datagen.h"
#include "evaluationClassical2.h"
#include "magics.h"

void Run(int argc, char** argv)
{
    std::cout << "Chess.cpp by Gediminas Masaitis" << std::endl;
    std::cout << "Type \"help\" for more info" << std::endl;
    Evaluation::Init();
    if(argc > 1)
    {
        std::string param1 = argv[1];
        if(param1 == "bench")
        {
            Bench::Run();
            return;
        }

        if (param1 == "datagen")
        {
            DataGenerator::Run();
            return;
        }
    }
    
    auto uci = UciStd();
    uci.Run();
}

void Test()
{
    Evaluation::Init();

    //std::cout << sizeof(TranspositionTableEntry) << std::endl;

    //Game::RunTestCommands();
    //Game::RunCuteChessOutput();

    //Tests::TestSee();
    //Tests::TestMagics();
    //Tests::TestZobrist();
    //Tests::TestRepetitions();
    //Tests::TestUci();
    //Tests::TestEval();
    //Tests::TestTablebases();
    Tests::TestUciFen();
    
    //Tests::TestSfen();
    //Tests::TestFenSerialize();
    //Tests::TestPgnParse();
    
    //PerftRunner::Run(Uci::StartingFen, 6);
}


int main(int argc, char** argv)
{
    setbuf(stdout, 0);
    //Test();
    //Train();
    Run(argc, argv);

    return 0;
}
