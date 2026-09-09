module m;
  default clocking @(posedge clk);
  endclocking
  initial begin
  end
  initial begin
    expect (@(posedge clk) a ##1 b ##1 c ##1 c)
      f("");
  end
endmodule
