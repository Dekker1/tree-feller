// Words that are keywords in one position and identifiers in another, which is
// what the keyword re-lex and the word fallback exist for.
pragma solidity ^0.8.0;

interface IThing { function get() external view returns (uint256); }

contract Thing is IThing {
    error NotAllowed(address who);
    event Sent(address indexed from, uint256 value);

    mapping(address => uint256) private balances;
    uint256 public constant LIMIT = 1000;

    modifier onlyPositive(uint256 value) { require(value > 0); _; }

    function get() external view override returns (uint256) { return LIMIT; }

    function send(address to, uint256 value) public onlyPositive(value) {
        if (balances[msg.sender] < value) { revert NotAllowed(msg.sender); }
        unchecked { balances[msg.sender] -= value; }
        balances[to] += value;
        emit Sent(msg.sender, value);
    }
}
