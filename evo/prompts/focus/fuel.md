Computation: each turn has a fuel budget (100k for C++, 2x on the first game turn, 10x on the first draft
turn). A bot that runs out is suspended mid-turn and resumes next round. Cache map analysis, avoid
recomputing distance fields every turn, and measure fuel with the SDK's fuel_remaining().
