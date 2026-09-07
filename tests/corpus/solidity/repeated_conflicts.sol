pragma solidity ^0.8.0;

// Keep several independent ambiguity points in one parse. The split driver
// reuses branch and comparison storage after each one, including when a later
// conflict needs a different number of branches or a longer action log.
contract RepeatedConflicts {
    mapping(address => uint256) private balances;
    mapping(address => mapping(bytes32 => uint256)) private nested;

    event Sent(address indexed from, address indexed to, uint256 value);

    function send(address to, uint256 value) public returns (uint256) {
        require(value > 0);
        unchecked { balances[msg.sender] -= value; }
        balances[to] += value;
        emit Sent(msg.sender, to, value);
        return balances[to];
    }

    function update(bytes32 key, uint256[] memory values) public {
        for (uint256 i = 0; i < values.length; i++) {
            nested[msg.sender][key] += values[i];
        }
    }

    function choose(bool condition, uint256 left, uint256 right)
        public
        pure
        returns (uint256)
    {
        return condition ? left : right;
    }
}
