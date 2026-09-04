contract C {
    struct S { uint256 from; uint256 error; uint256 revert; }
    function f() public pure returns (uint256) {
        uint256 from = 1;
        return from;
    }
}
