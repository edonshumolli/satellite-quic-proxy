----------------------------------------------------------------------------------
-- Testbench for QUIC Packet Processor Module
-- 
-- This testbench validates the functionality of the QUIC packet processor module
-- by simulating packet framing, processing ACKs, and retransmission handling.
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;
use STD.TEXTIO.ALL;
use IEEE.STD_LOGIC_TEXTIO.ALL;

entity quic_packet_processor_tb is
-- Testbench has no ports
end quic_packet_processor_tb;

architecture Behavioral of quic_packet_processor_tb is
    -- Component Declaration for the Unit Under Test (UUT)
    component quic_packet_processor
        Port (
            clk               : in  STD_LOGIC;
            rst               : in  STD_LOGIC;
            start             : in  STD_LOGIC;
            mode              : in  STD_LOGIC_VECTOR(1 downto 0);
            operation         : in  STD_LOGIC_VECTOR(1 downto 0);
            done              : out STD_LOGIC;
            connection_id     : in  STD_LOGIC_VECTOR(63 downto 0);
            packet_number     : in  STD_LOGIC_VECTOR(31 downto 0);
            data_in           : in  STD_LOGIC_VECTOR(63 downto 0);
            data_in_valid     : in  STD_LOGIC;
            data_in_last      : in  STD_LOGIC;
            data_in_ready     : out STD_LOGIC;
            data_out          : out STD_LOGIC_VECTOR(63 downto 0);
            data_out_valid    : out STD_LOGIC;
            data_out_last     : out STD_LOGIC;
            data_out_ready    : in  STD_LOGIC;
            ack_packet_num    : in  STD_LOGIC_VECTOR(31 downto 0);
            ack_receive       : in  STD_LOGIC;
            ack_send          : out STD_LOGIC;
            ack_ranges        : in  STD_LOGIC_VECTOR(127 downto 0);
            retx_needed       : out STD_LOGIC;
            retx_packet_num   : out STD_LOGIC_VECTOR(31 downto 0);
            retx_timeout      : in  STD_LOGIC_VECTOR(15 downto 0);
            packets_sent      : out STD_LOGIC_VECTOR(31 downto 0);
            packets_received  : out STD_LOGIC_VECTOR(31 downto 0);
            packets_retx      : out STD_LOGIC_VECTOR(31 downto 0)
        );
    end component;
    
    -- Clock and reset signals
    signal clk           : STD_LOGIC := '0';
    signal rst           : STD_LOGIC := '1';
    
    -- Control signals
    signal start         : STD_LOGIC := '0';
    signal mode          : STD_LOGIC_VECTOR(1 downto 0) := "00";
    signal operation     : STD_LOGIC_VECTOR(1 downto 0) := "00";
    signal done          : STD_LOGIC;
    
    -- QUIC configuration
    signal connection_id : STD_LOGIC_VECTOR(63 downto 0) := x"0123456789ABCDEF";
    signal packet_number : STD_LOGIC_VECTOR(31 downto 0) := x"00000001";
    
    -- Data interface signals
    signal data_in       : STD_LOGIC_VECTOR(63 downto 0) := (others => '0');
    signal data_in_valid : STD_LOGIC := '0';
    signal data_in_last  : STD_LOGIC := '0';
    signal data_in_ready : STD_LOGIC;
    signal data_out      : STD_LOGIC_VECTOR(63 downto 0);
    signal data_out_valid: STD_LOGIC;
    signal data_out_last : STD_LOGIC;
    signal data_out_ready: STD_LOGIC := '0';
    
    -- ACK signals
    signal ack_packet_num: STD_LOGIC_VECTOR(31 downto 0) := x"00000001";
    signal ack_receive   : STD_LOGIC := '0';
    signal ack_send      : STD_LOGIC;
    signal ack_ranges    : STD_LOGIC_VECTOR(127 downto 0) := (others => '0');
    
    -- Retransmission signals
    signal retx_needed   : STD_LOGIC;
    signal retx_packet_num : STD_LOGIC_VECTOR(31 downto 0);
    signal retx_timeout  : STD_LOGIC_VECTOR(15 downto 0) := x"0064"; -- 100ms timeout
    
    -- Statistics
    signal packets_sent  : STD_LOGIC_VECTOR(31 downto 0);
    signal packets_received : STD_LOGIC_VECTOR(31 downto 0);
    signal packets_retx  : STD_LOGIC_VECTOR(31 downto 0);
    
    -- Clock period definition
    constant clk_period  : time := 10 ns;
    
    -- Test data constants
    type data_array is array (natural range <>) of STD_LOGIC_VECTOR(63 downto 0);
    
    -- Initialize with test pattern data
    signal TEST_DATA : data_array(0 to 3) := (
        x"0123456789ABCDEF",
        x"FEDCBA9876543210",
        x"AAAAAAAAAAAAAAAAA",
        x"BBBBBBBBBBBBBBBB"
    );
    
    -- Variables to store output packets
    signal output_data : data_array(0 to 9) := (others => (others => '0'));
    signal output_count : integer := 0;
    
    -- Procedure for sending data blocks
    procedure send_data_blocks (
        constant data_blocks : in data_array;
        constant block_count : in integer;
        signal data          : out STD_LOGIC_VECTOR(63 downto 0);
        signal valid         : out STD_LOGIC;
        signal last          : out STD_LOGIC;
        signal ready         : in  STD_LOGIC
    ) is
    begin
        for i in 0 to block_count-1 loop
            wait until rising_edge(clk);
            data <= data_blocks(i);
            valid <= '1';
            if i = block_count-1 then
                last <= '1';
            else
                last <= '0';
            end if;
            
            -- Wait until ready
            wait until ready = '1';
            wait until rising_edge(clk);
        end loop;
        
        -- Reset signals
        valid <= '0';
        last <= '0';
    end procedure;
    
    -- Procedure for receiving data blocks
    procedure receive_data_blocks (
        signal data_array    : out data_array;
        signal count         : out integer;
        signal ready         : out STD_LOGIC;
        signal valid         : in  STD_LOGIC;
        signal last          : in  STD_LOGIC;
        signal data          : in  STD_LOGIC_VECTOR(63 downto 0)
    ) is
        variable local_count : integer := 0;
    begin
        count <= 0;
        ready <= '1';
        
        while true loop
            wait until rising_edge(clk);
            if valid = '1' then
                data_array(local_count) <= data;
                local_count := local_count + 1;
                count <= local_count;
                
                if last = '1' then
                    exit;
                end if;
            end if;
        end loop;
        
        ready <= '0';
    end procedure;
    
begin
    -- Instantiate the Unit Under Test (UUT)
    uut: quic_packet_processor
    port map (
        clk             => clk,
        rst             => rst,
        start           => start,
        mode            => mode,
        operation       => operation,
        done            => done,
        connection_id   => connection_id,
        packet_number   => packet_number,
        data_in         => data_in,
        data_in_valid   => data_in_valid,
        data_in_last    => data_in_last,
        data_in_ready   => data_in_ready,
        data_out        => data_out,
        data_out_valid  => data_out_valid,
        data_out_last   => data_out_last,
        data_out_ready  => data_out_ready,
        ack_packet_num  => ack_packet_num,
        ack_receive     => ack_receive,
        ack_send        => ack_send,
        ack_ranges      => ack_ranges,
        retx_needed     => retx_needed,
        retx_packet_num => retx_packet_num,
        retx_timeout    => retx_timeout,
        packets_sent    => packets_sent,
        packets_received => packets_received,
        packets_retx    => packets_retx
    );
    
    -- Clock process definition
    clk_process: process
    begin
        clk <= '0';
        wait for clk_period/2;
        clk <= '1';
        wait for clk_period/2;
    end process;
    
    -- Stimulus process
    stim_proc: process
    begin
        -- Initialize inputs
        rst <= '1';
        start <= '0';
        mode <= "00";  -- Initial packet
        operation <= "00";  -- Framing operation
        data_in <= (others => '0');
        data_in_valid <= '0';
        data_in_last <= '0';
        data_out_ready <= '0';
        ack_receive <= '0';
        
        -- Hold reset for 100 ns
        wait for 100 ns;
        rst <= '0';
        wait for clk_period*10;
        
        -- Test 1: Packet Framing
        report "Starting packet framing test with stream multiplexing...";
        mode <= "10";  -- Application data packet
        operation <= "00";  -- Framing operation
        
        -- Start framing for Stream 0
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Send data to frame for Stream 0
        send_data_blocks(TEST_DATA, 2, data_in, data_in_valid, data_in_last, data_in_ready);
        
        -- Receive framed packets
        receive_data_blocks(output_data, output_count, data_out_ready, data_out_valid, data_out_last, data_out);
        
        -- Wait for completion
        wait until done = '1';
        wait for clk_period*10;
        
        -- Start framing for a new stream (Stream 1)
        report "Sending data for a new stream...";
        mode <= "10";  -- Application data packet
        operation <= "00";  -- Framing operation
        
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Send data to frame for Stream 1
        send_data_blocks(TEST_DATA(2 to 3), 2, data_in, data_in_valid, data_in_last, data_in_ready);
        
        -- Receive framed packets
        receive_data_blocks(output_data, output_count, data_out_ready, data_out_valid, data_out_last, data_out);
        
        -- Wait for completion
        wait until done = '1';
        wait for clk_period*10;
        
        -- Test 1b: Process Stream Frame (Simulating receiving a stream frame)
        report "Testing stream frame processing...";
        mode <= "10";  -- Application data packet
        operation <= "00";  -- For this simplified test, reuse framing operation
        
        -- Prepare test data that looks like a STREAM frame
        -- We'll manually construct a stream frame in the test data
        -- Frame type + Stream ID + Offset + Length + Payload
        TEST_DATA(0) <= x"0A00000000000000";  -- STREAM frame type (0x0A = FIN=0, LEN=1, OFF=1) + first byte of Stream ID
        TEST_DATA(1) <= x"0000000000000000";  -- Rest of Stream ID (0) and start of offset
        TEST_DATA(2) <= x"0000001000000000";  -- Rest of offset (0) and length (16 bytes)
        TEST_DATA(3) <= x"FFFFFFFFFFFFFFFF";  -- Payload data
        
        -- Start processing
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Send the simulated stream frame
        send_data_blocks(TEST_DATA, 4, data_in, data_in_valid, data_in_last, data_in_ready);
        
        -- Wait for completion
        wait until done = '1';
        wait for clk_period*10;
        
        -- Report framing results
        report "Stream processing test completed";
        report "Total input data blocks: 8";  -- 4 blocks x 2 tests
        report "Total output packet blocks: " & integer'image(output_count);
        report "Packets sent counter: " & integer'image(to_integer(unsigned(packets_sent)));
        
        -- Test 1c: Header Validation (testing invalid header)
        report "Testing invalid header validation...";
        mode <= "10";  -- Application data packet
        operation <= "00";  -- Frame operation
        
        -- Prepare test data with invalid header
        -- Invalid packet type in first byte
        TEST_DATA(0) <= x"7000000000000000";  -- Invalid packet type (not a valid QUIC long or short header)
        TEST_DATA(1) <= x"0000000000000000";
        TEST_DATA(2) <= x"0000000000000000";
        TEST_DATA(3) <= x"0000000000000000";
        
        -- Start processing
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Send the invalid header data
        send_data_blocks(TEST_DATA, 4, data_in, data_in_valid, data_in_last, data_in_ready);
        
        -- Wait for completion (validation should fail and go to COMPLETE state)
        wait until done = '1';
        wait for clk_period*10;
        
        report "Header validation test completed";
        
        -- Test 1d: Invalid Frame Type Validation
        report "Testing invalid frame type validation...";
        mode <= "10";  -- Application data packet
        operation <= "00";  -- Frame operation
        
        -- Prepare test data with valid header but invalid frame type
        TEST_DATA(0) <= x"C000000000000000";  -- Valid QUIC long header
        TEST_DATA(1) <= x"0000000100000000";  -- Version 1
        TEST_DATA(2) <= x"0800000000000000";  -- DCID length 8 bytes
        TEST_DATA(3) <= x"FF00000000000000";  -- Invalid frame type (FF is not a valid QUIC frame type)
        
        -- Start processing
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Send the data with invalid frame type
        send_data_blocks(TEST_DATA, 4, data_in, data_in_valid, data_in_last, data_in_ready);
        
        -- Wait for completion (validation should fail and go to COMPLETE state)
        wait until done = '1';
        wait for clk_period*10;
        
        report "Frame type validation test completed";
        
        -- Test 2: ACK Processing
        report "Starting ACK processing test...";
        mode <= "10";  -- Application data packet
        operation <= "01";  -- ACK processing
        ack_packet_num <= x"00000001";
        ack_receive <= '1';
        
        -- Start ACK processing
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Wait for completion
        wait until done = '1';
        wait for clk_period*10;
        
        -- Report ACK results
        report "ACK processing test completed";
        report "ACK send signal: " & std_logic'image(ack_send);
        report "Packets received counter: " & integer'image(to_integer(unsigned(packets_received)));
        
        -- Test 3: Retransmission Check
        report "Starting retransmission check test...";
        mode <= "10";  -- Application data packet
        operation <= "10";  -- Retransmission check
        
        -- Simulate time passing to trigger timeout (would be done by the timer in the actual module)
        -- In a real implementation, we would wait for the timeout
        -- Here we're just starting the retransmission check operation
        
        -- Start retransmission check
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Wait for retransmission detection
        wait until done = '1';
        wait for clk_period*10;
        
        -- Report retransmission results
        report "Retransmission check test completed";
        if retx_needed = '1' then
            report "Retransmission needed for packet: " & 
                   integer'image(to_integer(unsigned(retx_packet_num)));
            report "Packets retransmitted counter: " & 
                   integer'image(to_integer(unsigned(packets_retx)));
        else
            report "No retransmission needed";
        end if;
        
        -- Wait for a bit before ending simulation
        wait for 100 ns;
        
        -- All tests completed
        report "All tests completed!";
        wait;
    end process;

end Behavioral;
