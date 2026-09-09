module m #();
  logic a; // Keep an extra between previously completed module items.
  wire b;
  always_ff @(clk) ;
  always_ff @(clk) if ((ready)) $display();
`endif
  always_comb begin begin end end
  T #((WIDTH)) u();
  T #(.W(), .D()) v((data));
endmodule
