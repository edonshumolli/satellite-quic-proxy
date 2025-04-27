----------------------------------------------------------------------------------
-- QUIC Packet Processing Module
-- 
-- This module implements hardware-accelerated packet framing, sequencing, and 
-- retransmission logic for QUIC protocol.
--
-- Features:
-- - Packet framing and header processing
-- - Sequence number tracking and generation
-- - ACK frame generation and processing
-- - Retransmission detection and handling
-- - Stream multiplexing and tracking (multiple streams per connection)
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity quic_packet_processor is
    Port (
        -- Clock and reset
        clk             : in  STD_LOGIC;
        rst             : in  STD_LOGIC;
        
        -- Control signals
        start           : in  STD_LOGIC;
        mode            : in  STD_LOGIC_VECTOR(1 downto 0);  -- 00: Initial, 01: Handshake, 10: Application, 11: Retry
        operation       : in  STD_LOGIC_VECTOR(1 downto 0);  -- 00: Frame, 01: Process ACK, 10: Retransmit, 11: Reserved
        done            : out STD_LOGIC;
        
        -- QUIC configuration
        connection_id   : in  STD_LOGIC_VECTOR(63 downto 0);
        packet_number   : in  STD_LOGIC_VECTOR(31 downto 0);
        
        -- Input data interface
        data_in         : in  STD_LOGIC_VECTOR(63 downto 0);
        data_in_valid   : in  STD_LOGIC;
        data_in_last    : in  STD_LOGIC;
        data_in_ready   : out STD_LOGIC;
        
        -- Output data interface
        data_out        : out STD_LOGIC_VECTOR(63 downto 0);
        data_out_valid  : out STD_LOGIC;
        data_out_last   : out STD_LOGIC;
        data_out_ready  : in  STD_LOGIC;
        
        -- ACK information
        ack_packet_num  : in  STD_LOGIC_VECTOR(31 downto 0);
        ack_receive     : in  STD_LOGIC;
        ack_send        : out STD_LOGIC;
        ack_ranges      : in  STD_LOGIC_VECTOR(127 downto 0);  -- Encoded ACK ranges
        
        -- Retransmission interface
        retx_needed     : out STD_LOGIC;
        retx_packet_num : out STD_LOGIC_VECTOR(31 downto 0);
        retx_timeout    : in  STD_LOGIC_VECTOR(15 downto 0);  -- Timeout in milliseconds
        
        -- Statistics
        packets_sent    : out STD_LOGIC_VECTOR(31 downto 0);
        packets_received: out STD_LOGIC_VECTOR(31 downto 0);
        packets_retx    : out STD_LOGIC_VECTOR(31 downto 0)
    );
end quic_packet_processor;

architecture Behavioral of quic_packet_processor is
    -- QUIC packet type constants
    constant PKT_TYPE_INITIAL    : STD_LOGIC_VECTOR(7 downto 0) := x"FF";
    constant PKT_TYPE_0RTT       : STD_LOGIC_VECTOR(7 downto 0) := x"FE";
    constant PKT_TYPE_HANDSHAKE  : STD_LOGIC_VECTOR(7 downto 0) := x"FD";
    constant PKT_TYPE_RETRY      : STD_LOGIC_VECTOR(7 downto 0) := x"FC";
    constant PKT_TYPE_VERSION_NEG: STD_LOGIC_VECTOR(7 downto 0) := x"FB";
    constant PKT_TYPE_APP_DATA   : STD_LOGIC_VECTOR(7 downto 0) := x"00";  -- 1-byte header form for short header
    
    -- Header validation error codes
    constant VALID_HEADER        : STD_LOGIC_VECTOR(3 downto 0) := x"0";  -- No error
    constant INVALID_PACKET_TYPE : STD_LOGIC_VECTOR(3 downto 0) := x"1";  -- Unknown packet type
    constant INVALID_VERSION     : STD_LOGIC_VECTOR(3 downto 0) := x"2";  -- Unsupported version
    constant INVALID_CID_LENGTH  : STD_LOGIC_VECTOR(3 downto 0) := x"3";  -- Connection ID length out of bounds
    constant INVALID_PACKET_SIZE : STD_LOGIC_VECTOR(3 downto 0) := x"4";  -- Packet too small for claimed header
    constant INVALID_FRAME_TYPE  : STD_LOGIC_VECTOR(3 downto 0) := x"5";  -- Unknown frame type
    constant INVALID_PKT_NUM_LEN : STD_LOGIC_VECTOR(3 downto 0) := x"6";  -- Invalid packet number length
    constant INVALID_CID_MATCH   : STD_LOGIC_VECTOR(3 downto 0) := x"7";  -- Connection ID doesn't match expected
    constant INVALID_TOKEN       : STD_LOGIC_VECTOR(3 downto 0) := x"8";  -- Invalid token in Initial packet
    
    -- QUIC frame type constants
    constant FRAME_TYPE_PADDING  : STD_LOGIC_VECTOR(7 downto 0) := x"00";
    constant FRAME_TYPE_PING     : STD_LOGIC_VECTOR(7 downto 0) := x"01";
    constant FRAME_TYPE_ACK      : STD_LOGIC_VECTOR(7 downto 0) := x"02";
    constant FRAME_TYPE_ACK_ECN  : STD_LOGIC_VECTOR(7 downto 0) := x"03";
    constant FRAME_TYPE_RESET    : STD_LOGIC_VECTOR(7 downto 0) := x"04";
    constant FRAME_TYPE_CRYPTO   : STD_LOGIC_VECTOR(7 downto 0) := x"06";
    constant FRAME_TYPE_STREAM   : STD_LOGIC_VECTOR(7 downto 0) := x"08";  -- Variable depending on flags
    
    -- STREAM frame flags (bits 2-0 of the frame type byte)
    -- 0x08 to 0x0F are valid stream frame types with different flag combinations
    -- Bit 0 (0x01): FIN flag - indicates this is the final frame for the stream
    -- Bit 1 (0x02): LEN flag - indicates length is present
    -- Bit 2 (0x04): OFF flag - indicates offset is present
    
    -- Buffer sizes
    constant BUFFER_SIZE : integer := 2048;
    
    -- Buffers
    type buffer_type is array (0 to BUFFER_SIZE-1) of STD_LOGIC_VECTOR(7 downto 0);
    signal input_buffer  : buffer_type;
    signal output_buffer : buffer_type;
    signal in_buf_wrptr  : integer range 0 to BUFFER_SIZE-1;
    signal in_buf_rdptr  : integer range 0 to BUFFER_SIZE-1;
    signal out_buf_wrptr : integer range 0 to BUFFER_SIZE-1;
    signal out_buf_rdptr : integer range 0 to BUFFER_SIZE-1;
    
    -- Packet tracking
    type packet_record is record
        packet_num   : STD_LOGIC_VECTOR(31 downto 0);
        size         : integer range 0 to BUFFER_SIZE;
        timestamp    : STD_LOGIC_VECTOR(31 downto 0);
        acked        : STD_LOGIC;
        retx_count   : integer range 0 to 15;
    end record;
    
    constant MAX_TRACKED_PACKETS : integer := 128;
    type packet_tracking_array is array (0 to MAX_TRACKED_PACKETS-1) of packet_record;
    signal packet_tracker : packet_tracking_array;
    signal packet_index   : integer range 0 to MAX_TRACKED_PACKETS-1;
    signal current_time   : STD_LOGIC_VECTOR(31 downto 0);  -- In milliseconds
    
    -- Internal counters
    signal packet_count_sent     : unsigned(31 downto 0);
    signal packet_count_received : unsigned(31 downto 0);
    signal packet_count_retx     : unsigned(31 downto 0);
    
    -- State machine definition
    type state_type is (IDLE, LOAD_DATA, VALIDATE_HEADER, CREATE_HEADER, PROCESS_PAYLOAD, 
                         PROCESS_STREAM_FRAME, FRAME_DATA, PROCESS_ACK, CHECK_RETRANSMIT, 
                         RETRANSMIT_PACKET, OUTPUT_DATA, COMPLETE);
                         
    -- Header validation signals
    signal header_valid : STD_LOGIC;
    signal validation_error : STD_LOGIC_VECTOR(3 downto 0); -- Error code for different validation issues
    signal state, next_state : state_type;
    
    -- Header construction signals
    signal header_size      : integer range 0 to 63;
    signal header_buf       : STD_LOGIC_VECTOR(511 downto 0);  -- Large enough for any QUIC header
    signal packet_type      : STD_LOGIC_VECTOR(7 downto 0);
    signal final_packet_num : STD_LOGIC_VECTOR(31 downto 0);
    
    -- Timer for retransmissions
    signal timer_1ms : unsigned(15 downto 0);
    
    -- Stream multiplexing support
    constant MAX_STREAMS : integer := 64;  -- Maximum number of concurrent streams to track
    
    -- Stream state record type
    type stream_state_type is (
        STREAM_IDLE,      -- No data received or sent
        STREAM_OPEN,      -- Stream is open and active
        STREAM_CLOSED,    -- Stream has been closed (received FIN)
        STREAM_RESET      -- Stream has been reset
    );
    
    -- Stream record to track individual streams
    type stream_record is record
        stream_id     : STD_LOGIC_VECTOR(31 downto 0);  -- Stream identifier
        state         : stream_state_type;              -- Current stream state
        recv_offset   : STD_LOGIC_VECTOR(63 downto 0);  -- Largest received byte offset
        send_offset   : STD_LOGIC_VECTOR(63 downto 0);  -- Largest sent byte offset
        is_bidi       : STD_LOGIC;                      -- Bidirectional stream flag
        last_activity : STD_LOGIC_VECTOR(31 downto 0);  -- Timestamp of last activity
        fin_received  : STD_LOGIC;                      -- FIN flag received
        fin_sent      : STD_LOGIC;                      -- FIN flag sent
    end record;
    
    -- Array of stream records for tracking multiple streams
    type stream_array is array (0 to MAX_STREAMS-1) of stream_record;
    signal streams : stream_array;
    signal stream_count : integer range 0 to MAX_STREAMS;
    signal current_stream_id : STD_LOGIC_VECTOR(31 downto 0);  -- Current stream being processed
    
begin
    -- Main state machine process
    process(clk, rst)
    begin
        if rst = '1' then
            state <= IDLE;
            done <= '0';
            data_in_ready <= '0';
            data_out_valid <= '0';
            data_out_last <= '0';
            in_buf_wrptr <= 0;
            in_buf_rdptr <= 0;
            out_buf_wrptr <= 0;
            out_buf_rdptr <= 0;
            packet_index <= 0;
            packet_count_sent <= (others => '0');
            packet_count_received <= (others => '0');
            packet_count_retx <= (others => '0');
            packets_sent <= (others => '0');
            packets_received <= (others => '0');
            packets_retx <= (others => '0');
            current_time <= (others => '0');
            timer_1ms <= (others => '0');
            retx_needed <= '0';
            ack_send <= '0';
            
            -- Initialize header validation signals
            header_valid <= '0';
            validation_error <= VALID_HEADER;
            
            -- Initialize stream multiplexing tracking
            stream_count <= 0;
            current_stream_id <= (others => '0');
            
            -- Initialize all stream records
            for i in 0 to MAX_STREAMS-1 loop
                streams(i).stream_id <= (others => '0');
                streams(i).state <= STREAM_IDLE;
                streams(i).recv_offset <= (others => '0');
                streams(i).send_offset <= (others => '0');
                streams(i).is_bidi <= '0';
                streams(i).last_activity <= (others => '0');
                streams(i).fin_received <= '0';
                streams(i).fin_sent <= '0';
            end loop;
        elsif rising_edge(clk) then
            -- 1ms timer for packet timeout tracking
            if timer_1ms = 50000 then  -- Assuming 50 MHz clock (20ns period)
                timer_1ms <= (others => '0');
                current_time <= STD_LOGIC_VECTOR(unsigned(current_time) + 1);
            else
                timer_1ms <= timer_1ms + 1;
            end if;
            
            state <= next_state;
            
            case state is
                when IDLE =>
                    done <= '0';
                    retx_needed <= '0';
                    ack_send <= '0';
                    
                    if start = '1' then
                        data_in_ready <= '1';
                        data_out_valid <= '0';
                        
                        -- Determine packet type based on mode
                        case mode is
                            when "00" => packet_type <= PKT_TYPE_INITIAL;
                            when "01" => packet_type <= PKT_TYPE_HANDSHAKE;
                            when "10" => packet_type <= PKT_TYPE_APP_DATA;
                            when "11" => packet_type <= PKT_TYPE_RETRY;
                            when others => packet_type <= PKT_TYPE_APP_DATA;
                        end case;
                        
                        -- Set operation type
                        case operation is
                            when "00" => next_state <= LOAD_DATA;       -- Frame
                            when "01" => next_state <= PROCESS_ACK;     -- Process ACK
                            when "10" => next_state <= CHECK_RETRANSMIT; -- Retransmit
                            when others => next_state <= IDLE;
                        end case;
                    end if;
                
                when LOAD_DATA =>
                    if data_in_valid = '1' and data_in_ready = '1' then
                        -- Load 64-bit word into input buffer, byte by byte
                        for i in 0 to 7 loop
                            input_buffer(in_buf_wrptr) <= data_in((i*8+7) downto (i*8));
                            in_buf_wrptr <= (in_buf_wrptr + 1) mod BUFFER_SIZE;
                        end loop;
                        
                        if data_in_last = '1' then
                            -- Last data block received
                            data_in_ready <= '0';
                            -- First validate the header before creating/processing
                            next_state <= VALIDATE_HEADER;
                        end if;
                    end if;
                
                when VALIDATE_HEADER =>
                    -- Perform header validation on the received packet data
                    -- Reset validation state
                    header_valid <= '1';  -- Default to valid
                    validation_error <= VALID_HEADER;
                    
                    -- Check packet type byte (first byte in the buffer)
                    if in_buf_wrptr > 0 then
                        -- First check if this is a long header or short header packet 
                        -- Long header: most significant bit is 1 (0x80)
                        -- Short header: most significant bit is 0
                        if (input_buffer(0)(7) = '1') then
                            -- Long header validation
                            
                            -- Check for valid packet type in the first byte
                            -- Long headers have specific types in the lower 7 bits
                            if not (
                                (input_buffer(0) = PKT_TYPE_INITIAL) or
                                (input_buffer(0) = PKT_TYPE_0RTT) or
                                (input_buffer(0) = PKT_TYPE_HANDSHAKE) or
                                (input_buffer(0) = PKT_TYPE_RETRY) or
                                (input_buffer(0) = PKT_TYPE_VERSION_NEG)
                            ) then
                                header_valid <= '0';
                                validation_error <= INVALID_PACKET_TYPE;
                            end if;
                            
                            -- Check for minimum valid packet size
                            -- Long headers need at least: 
                            -- 1 byte header form + 4 bytes version + 1 byte DCID len + 1 byte SCID len
                            if (in_buf_wrptr < 7) then
                                header_valid <= '0';
                                validation_error <= INVALID_PACKET_SIZE;
                            end if;
                            
                            -- Check QUIC version (supported versions)
                            -- For now, only support version 1
                            if (in_buf_wrptr >= 5) then
                                if not (
                                    (input_buffer(1) = x"00") and 
                                    (input_buffer(2) = x"00") and 
                                    (input_buffer(3) = x"00") and 
                                    (input_buffer(4) = x"01")
                                ) then
                                    header_valid <= '0';
                                    validation_error <= INVALID_VERSION;
                                end if;
                            end if;
                            
                            -- Check Connection ID lengths are valid
                            if (in_buf_wrptr >= 6) then
                                -- DCID length check: Max allowed is 20 bytes
                                if (to_integer(unsigned(input_buffer(5))) > 20) then
                                    header_valid <= '0';
                                    validation_error <= INVALID_CID_LENGTH;
                                end if;
                                
                                -- Calculate the offset to SCID length field based on DCID length
                                -- SCID length is located after: 1-byte header + 4-byte version + 1-byte DCID len + DCID bytes
                                if (in_buf_wrptr >= (6 + to_integer(unsigned(input_buffer(5))))) then
                                    -- Get the index of the SCID length byte
                                    -- header(0) + version(4) + DCID_len(1) + DCID_bytes
                                    variable scid_len_index : integer := 6 + to_integer(unsigned(input_buffer(5)));
                                    
                                    -- Check SCID length validity (max 20 bytes)
                                    if (to_integer(unsigned(input_buffer(scid_len_index))) > 20) then
                                        header_valid <= '0';
                                        validation_error <= INVALID_CID_LENGTH;
                                    end if;
                                    
                                    -- If this is an Initial packet, check for token
                                    if (input_buffer(0) = PKT_TYPE_INITIAL) then
                                        -- Token length field follows the SCID field
                                        -- Calculate token length offset: header + version + DCID_len + DCID + SCID_len + SCID
                                        variable token_len_index : integer := scid_len_index + 1 + 
                                                                              to_integer(unsigned(input_buffer(scid_len_index)));
                                        
                                        -- For Initial packets, we require a valid token (for DoS protection)
                                        -- In a real implementation, you would validate the token
                                        -- For now, we'll just verify that the token length field exists
                                        if (in_buf_wrptr < token_len_index) then
                                            header_valid <= '0';
                                            validation_error <= INVALID_TOKEN;
                                        end if;
                                    end if;
                                end if;
                                
                                -- Verify the CID matches our expected connection ID (simplified)
                                -- In a real implementation, you would compare against a connection table
                                -- Here we just verify that the packet belongs to our connection
                                if (header_valid = '1' and to_integer(unsigned(input_buffer(5))) = 8) then
                                    -- 8-byte connection ID
                                    variable cid_match : boolean := true;
                                    
                                    -- Compare connection ID bytes (assuming 8-byte CID)
                                    for i in 0 to 7 loop
                                        if (input_buffer(6 + i) /= connection_id((i*8+7) downto (i*8))) then
                                            cid_match := false;
                                        end if;
                                    end loop;
                                    
                                    if not cid_match then
                                        header_valid <= '0';
                                        validation_error <= INVALID_CID_MATCH;
                                    end if;
                                end if;
                            end if;
                        else
                            -- Short header validation
                            
                            -- Header must have at least 1 byte header form + CID + packet number
                            -- Minimum short header: 1 byte header + 8 byte CID + 1 byte packet number = 10 bytes
                            if (in_buf_wrptr < 10) then
                                header_valid <= '0';
                                validation_error <= INVALID_PACKET_SIZE;
                            end if;
                            
                            -- For short headers, the rightmost 5 bits are reserved and must be set to 0
                            if (input_buffer(0)(4 downto 0) /= "00000") then
                                header_valid <= '0';
                                validation_error <= INVALID_PACKET_TYPE;
                            end if;
                            
                            -- Bits 5-6 of the first byte indicate packet number length
                            -- 00: 1 byte, 01: 2 bytes, 10: 3 bytes, 11: 4 bytes
                            -- Check packets have enough bytes for the claimed packet number length
                            -- Extract packet number length bits
                            variable pkt_num_len : integer := 1; -- Default 1 byte
                            
                            case input_buffer(0)(6 downto 5) is
                                when "00" => pkt_num_len := 1;
                                when "01" => pkt_num_len := 2;
                                when "10" => pkt_num_len := 3;
                                when "11" => pkt_num_len := 4;
                                when others => pkt_num_len := 1;
                            end case;
                            
                            -- Verify we have enough bytes for: 1-byte header + 8-byte CID + packet number
                            if (in_buf_wrptr < (1 + 8 + pkt_num_len)) then
                                header_valid <= '0';
                                validation_error <= INVALID_PKT_NUM_LEN;
                            end if;
                            
                            -- Verify the CID matches our expected connection ID
                            if (header_valid = '1') then
                                -- Connection ID follows the first byte in a short header
                                variable cid_match : boolean := true;
                                
                                -- Compare connection ID bytes
                                for i in 0 to 7 loop
                                    if (input_buffer(1 + i) /= connection_id((i*8+7) downto (i*8))) then
                                        cid_match := false;
                                    end if;
                                end loop;
                                
                                if not cid_match then
                                    header_valid <= '0';
                                    validation_error <= INVALID_CID_MATCH;
                                end if;
                            end if;
                        end if;
                    else
                        -- Buffer is empty, can't validate header
                        header_valid <= '0';
                        validation_error <= INVALID_PACKET_SIZE;
                    end if;
                
                when CREATE_HEADER =>
                    -- Create appropriate QUIC packet header based on packet_type
                    header_size <= 0;
                    
                    if packet_type = PKT_TYPE_APP_DATA then
                        -- Short header format (1-RTT protected application data)
                        -- 1-byte header form + connection ID + packet number
                        header_buf(7 downto 0) <= packet_type;
                        header_size <= 1;
                        
                        -- Add connection ID (variable length, using full 8 bytes here)
                        header_buf(71 downto 8) <= connection_id;
                        header_size <= header_size + 8;
                        
                        -- Add packet number (variable length, using 4 bytes here)
                        final_packet_num <= packet_number;
                        header_buf(103 downto 72) <= packet_number;
                        header_size <= header_size + 4;
                    else
                        -- Long header format
                        -- 1-byte header form
                        header_buf(7 downto 0) <= packet_type;
                        header_size <= 1;
                        
                        -- 4-byte version field (using version 1)
                        header_buf(39 downto 8) <= x"00000001";
                        header_size <= header_size + 4;
                        
                        -- DCID length (1 byte) and DCID (variable)
                        header_buf(47 downto 40) <= x"08";  -- 8-byte CID
                        header_size <= header_size + 1;
                        
                        header_buf(111 downto 48) <= connection_id;
                        header_size <= header_size + 8;
                        
                        -- SCID length (1 byte) and SCID (variable) - Using same CID for simplicity
                        header_buf(119 downto 112) <= x"08";  -- 8-byte CID
                        header_size <= header_size + 1;
                        
                        header_buf(183 downto 120) <= connection_id;
                        header_size <= header_size + 8;
                        
                        if packet_type = PKT_TYPE_INITIAL then
                            -- Token Length (variable length integer, 1 byte for zero)
                            header_buf(191 downto 184) <= x"00";
                            header_size <= header_size + 1;
                        end if;
                        
                        -- Length (variable length integer, 2 bytes for simplicity)
                        -- Actual length would be calculated based on payload size
                        header_buf(207 downto 192) <= x"0100";  -- Placeholder, will be updated
                        header_size <= header_size + 2;
                        
                        -- Packet number (4 bytes)
                        final_packet_num <= packet_number;
                        header_buf(239 downto 208) <= packet_number;
                        header_size <= header_size + 4;
                    end if;
                    
                    next_state <= FRAME_DATA;
                
                when FRAME_DATA =>
                    -- Copy header to output buffer
                    for i in 0 to header_size-1 loop
                        output_buffer(out_buf_wrptr) <= header_buf((i*8+7) downto (i*8));
                        out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                    end loop;
                    
                    -- Frame payload data according to QUIC format with stream multiplexing support
                    if in_buf_wrptr > in_buf_rdptr then
                        -- Determine which stream to use
                        -- If the stream_count is 0, create a new stream (stream 0)
                        if stream_count = 0 then
                            current_stream_id <= x"00000000";  -- Start with stream 0
                            
                            -- Initialize the first stream
                            streams(0).stream_id <= x"00000000";
                            streams(0).state <= STREAM_OPEN;
                            streams(0).is_bidi <= '1';  -- Bidirectional stream
                            streams(0).last_activity <= current_time;
                            stream_count <= 1;
                        else
                            -- For subsequent packets, cycle through streams or use next available stream
                            -- For simplicity in this implementation, we'll cycle through existing streams
                            -- In a production system, stream allocation would follow QUIC protocol rules
                            
                            -- Find an active stream or create a new one if below max_streams
                            if stream_count < MAX_STREAMS then
                                current_stream_id <= STD_LOGIC_VECTOR(to_unsigned(stream_count, 32));
                                
                                -- Initialize the new stream
                                streams(stream_count).stream_id <= STD_LOGIC_VECTOR(to_unsigned(stream_count, 32));
                                streams(stream_count).state <= STREAM_OPEN;
                                streams(stream_count).is_bidi <= '1';  -- Bidirectional stream
                                streams(stream_count).last_activity <= current_time;
                                
                                stream_count <= stream_count + 1;
                            else
                                -- If at max streams, use round-robin approach
                                -- Find the least recently used active stream
                                current_stream_id <= streams(0).stream_id;  -- Default to first stream
                                
                                for i in 0 to MAX_STREAMS-1 loop
                                    if streams(i).state = STREAM_OPEN then
                                        current_stream_id <= streams(i).stream_id;
                                        streams(i).last_activity <= current_time;
                                        exit;
                                    end if;
                                end loop;
                            end if;
                        end if;
                        
                        -- Add STREAM frame header
                        -- STREAM frame type byte: 0x08 + flags
                        -- Bit 0 (0x01): FIN flag (0 for now)
                        -- Bit 1 (0x02): LEN flag (1 to include length)
                        -- Bit 2 (0x04): OFF flag (1 to include offset)
                        output_buffer(out_buf_wrptr) <= FRAME_TYPE_STREAM or x"0A";  -- FIN=0, LEN=1, OFF=1
                        out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                        
                        -- Stream ID (variable length integer, using 4 bytes to support larger stream IDs)
                        -- Note: For a full implementation, use variable length encoding
                        output_buffer(out_buf_wrptr) <= current_stream_id(7 downto 0);
                        out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                        output_buffer(out_buf_wrptr) <= current_stream_id(15 downto 8);
                        out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                        output_buffer(out_buf_wrptr) <= current_stream_id(23 downto 16);
                        out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                        output_buffer(out_buf_wrptr) <= current_stream_id(31 downto 24);
                        out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                        
                        -- Find the stream index based on current_stream_id
                        for i in 0 to MAX_STREAMS-1 loop
                            if streams(i).stream_id = current_stream_id then
                                -- Offset (variable length integer, using 8 bytes)
                                -- Using the current send_offset for the stream
                                for j in 0 to 7 loop
                                    output_buffer(out_buf_wrptr) <= streams(i).send_offset((j*8+7) downto (j*8));
                                    out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                                end loop;
                                
                                -- Update the send_offset for this stream after this transmission
                                -- Add payload size to current offset
                                streams(i).send_offset <= STD_LOGIC_VECTOR(unsigned(streams(i).send_offset) + 
                                                           to_unsigned((in_buf_wrptr - in_buf_rdptr), 64));
                                
                                -- Update last activity timestamp
                                streams(i).last_activity <= current_time;
                                exit;
                            end if;
                        end loop;
                        
                        -- Length (variable length integer, 2 bytes)
                        output_buffer(out_buf_wrptr) <= STD_LOGIC_VECTOR(to_unsigned((in_buf_wrptr - in_buf_rdptr) mod 256, 8));
                        out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                        output_buffer(out_buf_wrptr) <= STD_LOGIC_VECTOR(to_unsigned((in_buf_wrptr - in_buf_rdptr) / 256, 8));
                        out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                        
                        -- Copy payload data
                        while in_buf_rdptr /= in_buf_wrptr loop
                            output_buffer(out_buf_wrptr) <= input_buffer(in_buf_rdptr);
                            out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                            in_buf_rdptr <= (in_buf_rdptr + 1) mod BUFFER_SIZE;
                        end loop;
                    else
                        -- Add PING frame if no payload
                        output_buffer(out_buf_wrptr) <= FRAME_TYPE_PING;
                        out_buf_wrptr <= (out_buf_wrptr + 1) mod BUFFER_SIZE;
                    end if;
                    
                    -- Store packet for potential retransmission
                    if packet_index < MAX_TRACKED_PACKETS then
                        packet_tracker(packet_index).packet_num <= final_packet_num;
                        packet_tracker(packet_index).size <= out_buf_wrptr;
                        packet_tracker(packet_index).timestamp <= current_time;
                        packet_tracker(packet_index).acked <= '0';
                        packet_tracker(packet_index).retx_count <= 0;
                        packet_index <= (packet_index + 1) mod MAX_TRACKED_PACKETS;
                    end if;
                    
                    -- Update packet counter
                    packet_count_sent <= packet_count_sent + 1;
                    packets_sent <= STD_LOGIC_VECTOR(packet_count_sent + 1);
                    
                    next_state <= OUTPUT_DATA;
                
                when PROCESS_PAYLOAD =>
                    -- Process incoming packet payload, identify frame type and validate it
                    -- This is a simplified implementation assuming a single frame type
                    -- In a real implementation, you would parse multiple frames in a packet
                    
                    -- Reset validation state for frame type validation
                    header_valid <= '1';  -- Default to valid
                    validation_error <= VALID_HEADER;
                    
                    -- Examine the first byte in the input buffer to determine frame type
                    if in_buf_rdptr < in_buf_wrptr then
                        -- First check if this is a valid frame type
                        -- Valid frame types include: PADDING, PING, ACK, RESET, CRYPTO, STREAM, etc.
                        if (
                            (input_buffer(in_buf_rdptr) = FRAME_TYPE_PADDING) or
                            (input_buffer(in_buf_rdptr) = FRAME_TYPE_PING) or
                            (input_buffer(in_buf_rdptr) = FRAME_TYPE_ACK) or
                            (input_buffer(in_buf_rdptr) = FRAME_TYPE_ACK_ECN) or
                            (input_buffer(in_buf_rdptr) = FRAME_TYPE_RESET) or
                            (input_buffer(in_buf_rdptr) = FRAME_TYPE_CRYPTO) or
                            -- For STREAM frames, the frame type can be 0x08-0x0F due to flags
                            ((input_buffer(in_buf_rdptr) >= x"08") and (input_buffer(in_buf_rdptr) <= x"0F"))
                        ) then
                            -- Frame type is valid, proceed with processing
                            case input_buffer(in_buf_rdptr) is
                                when FRAME_TYPE_STREAM | x"09" | x"0A" | x"0B" | x"0C" | x"0D" | x"0E" | x"0F" =>
                                    -- Stream frame detected (any of 0x08-0x0F variations)
                                    next_state <= PROCESS_STREAM_FRAME;
                                    
                                when FRAME_TYPE_ACK | FRAME_TYPE_ACK_ECN =>
                                    -- ACK frame
                                    next_state <= PROCESS_ACK;
                                    
                                when others =>
                                    -- Other valid frame types - for now we'll just acknowledge them
                                    -- In a real implementation, you would handle each frame type
                                    -- Advance buffer read pointer past the frame
                                    in_buf_rdptr <= (in_buf_rdptr + 1) mod BUFFER_SIZE;
                                    next_state <= COMPLETE;
                            end case;
                        else
                            -- Invalid frame type
                            header_valid <= '0';
                            validation_error <= INVALID_FRAME_TYPE;
                            -- Skip to COMPLETE state with an error
                            next_state <= COMPLETE;
                        end if;
                    else
                        -- No data to process
                        next_state <= COMPLETE;
                    end if;
                
                when PROCESS_STREAM_FRAME =>
                    -- Process a STREAM frame and update the relevant stream record
                    -- Extract frame information: stream ID, offset, length, and FIN bit
                    
                    if in_buf_rdptr < in_buf_wrptr then
                        -- Read the frame type byte first
                        -- The frame type byte contains flags: FIN, LEN, OFF
                        -- 0x08 is the base type, with bits 0-2 used for flags
                        -- Bit 0 (0x01): FIN flag
                        -- Bit 1 (0x02): LEN flag
                        -- Bit 2 (0x04): OFF flag
                        
                        -- Check if there's enough data to process the header
                        if in_buf_wrptr - in_buf_rdptr >= 6 then  -- Minimum header size
                            -- Extract frame type and flags
                            -- Stream ID is at least 1 byte (variable length integer)
                            -- Note: This is a simplified implementation - full QUIC would use
                            -- variable length encoding for IDs, offsets, lengths, etc.
                            
                            -- Extract frame type (including flags)
                            -- Store in a variable for processing
                            variable frame_type_byte : STD_LOGIC_VECTOR(7 downto 0);
                            variable stream_offset : STD_LOGIC_VECTOR(63 downto 0);
                            variable data_length : integer range 0 to BUFFER_SIZE;
                            variable has_fin_flag : STD_LOGIC;
                            variable found_stream : boolean;
                            variable stream_idx : integer range 0 to MAX_STREAMS-1;
                            
                            begin
                                -- Read frame type byte
                                frame_type_byte := input_buffer(in_buf_rdptr);
                                in_buf_rdptr <= (in_buf_rdptr + 1) mod BUFFER_SIZE;
                                
                                -- Extract stream ID (using 4 bytes for simplicity)
                                -- In real implementation, you would use variable length decoding
                                current_stream_id(7 downto 0) <= input_buffer(in_buf_rdptr);
                                in_buf_rdptr <= (in_buf_rdptr + 1) mod BUFFER_SIZE;
                                current_stream_id(15 downto 8) <= input_buffer(in_buf_rdptr);
                                in_buf_rdptr <= (in_buf_rdptr + 1) mod BUFFER_SIZE;
                                current_stream_id(23 downto 16) <= input_buffer(in_buf_rdptr);
                                in_buf_rdptr <= (in_buf_rdptr + 1) mod BUFFER_SIZE;
                                current_stream_id(31 downto 24) <= input_buffer(in_buf_rdptr);
                                in_buf_rdptr <= (in_buf_rdptr + 1) mod BUFFER_SIZE;
                                
                                -- Initialize local variables
                                stream_offset := (others => '0');
                                data_length := 0;
                                has_fin_flag := frame_type_byte(0);
                                found_stream := false;
                                
                                -- Check for OFF flag (bit 2)
                                if (frame_type_byte(2) = '1') then
                                    -- Extract 8-byte offset if present
                                    -- This is simplified - real implementation uses variable length
                                    for j in 0 to 7 loop
                                        stream_offset((j*8+7) downto (j*8)) := input_buffer(in_buf_rdptr);
                                        in_buf_rdptr <= (in_buf_rdptr + 1) mod BUFFER_SIZE;
                                    end loop;
                                end if;
                                
                                -- Check for LEN flag (bit 1)
                                if (frame_type_byte(1) = '1') then
                                    -- Extract data length (2 bytes for simplicity)
                                    data_length := to_integer(unsigned(input_buffer(in_buf_rdptr))) + 
                                                  256 * to_integer(unsigned(input_buffer((in_buf_rdptr + 1) mod BUFFER_SIZE)));
                                    in_buf_rdptr <= (in_buf_rdptr + 2) mod BUFFER_SIZE;
                                else
                                    -- If no length field, data extends to end of packet
                                    data_length := in_buf_wrptr - in_buf_rdptr;
                                end if;
                                
                                -- Now find the stream in our tracking array or create a new one
                                found_stream := false;
                                for j in 0 to stream_count-1 loop
                                    if streams(j).stream_id = current_stream_id then
                                        -- Found the stream - update its record
                                        found_stream := true;
                                        stream_idx := j;
                                        
                                        -- Check if this data is newer than what we already have
                                        -- (higher offset + length)
                                        if (unsigned(stream_offset) + data_length) > unsigned(streams(j).recv_offset) then
                                            streams(j).recv_offset <= STD_LOGIC_VECTOR(unsigned(stream_offset) + data_length);
                                        end if;
                                        
                                        -- Update state and last activity
                                        streams(j).last_activity <= current_time;
                                        
                                        -- Check for FIN flag
                                        if has_fin_flag = '1' then
                                            streams(j).fin_received <= '1';
                                            if streams(j).state = STREAM_OPEN then
                                                streams(j).state <= STREAM_CLOSED;
                                            end if;
                                        end if;
                                        
                                        exit;
                                    end if;
                                end loop;
                                
                                -- If stream not found and we have room, create a new one
                                if not found_stream and stream_count < MAX_STREAMS then
                                    streams(stream_count).stream_id <= current_stream_id;
                                    streams(stream_count).state <= STREAM_OPEN;
                                    streams(stream_count).recv_offset <= STD_LOGIC_VECTOR(unsigned(stream_offset) + data_length);
                                    streams(stream_count).send_offset <= (others => '0');
                                    streams(stream_count).is_bidi <= '1';  -- Assuming bidirectional by default
                                    streams(stream_count).last_activity <= current_time;
                                    streams(stream_count).fin_received <= has_fin_flag;
                                    streams(stream_count).fin_sent <= '0';
                                    
                                    if has_fin_flag = '1' then
                                        streams(stream_count).state <= STREAM_CLOSED;
                                    end if;
                                    
                                    stream_count <= stream_count + 1;
                                end if;
                            
                            -- Skip past the stream data for now
                            -- In a real implementation, you would process this data
                            in_buf_rdptr <= (in_buf_rdptr + data_length) mod BUFFER_SIZE;
                        end if;
                    end if;
                    
                    next_state <= COMPLETE;
                
                when PROCESS_ACK =>
                    -- Process received ACK frame
                    if ack_receive = '1' then
                        -- Mark the acknowledged packet as received
                        for i in 0 to MAX_TRACKED_PACKETS-1 loop
                            if packet_tracker(i).packet_num = ack_packet_num then
                                packet_tracker(i).acked <= '1';
                                exit;
                            end if;
                        end loop;
                        
                        -- Update packet received counter
                        packet_count_received <= packet_count_received + 1;
                        packets_received <= STD_LOGIC_VECTOR(packet_count_received + 1);
                    end if;
                    
                    -- Generate an ACK frame if needed
                    ack_send <= '1';
                    next_state <= COMPLETE;
                
                when CHECK_RETRANSMIT =>
                    -- Check for packets that need retransmission
                    retx_needed <= '0';
                    
                    for i in 0 to MAX_TRACKED_PACKETS-1 loop
                        if packet_tracker(i).acked = '0' and 
                           packet_tracker(i).retx_count < 10 and  -- Max retransmissions
                           unsigned(current_time) - unsigned(packet_tracker(i).timestamp) > unsigned(retx_timeout) then
                            -- Packet needs retransmission
                            retx_needed <= '1';
                            retx_packet_num <= packet_tracker(i).packet_num;
                            packet_tracker(i).timestamp <= current_time;
                            packet_tracker(i).retx_count <= packet_tracker(i).retx_count + 1;
                            
                            -- Update retransmission counter
                            packet_count_retx <= packet_count_retx + 1;
                            packets_retx <= STD_LOGIC_VECTOR(packet_count_retx + 1);
                            
                            next_state <= RETRANSMIT_PACKET;
                            exit;
                        end if;
                    end loop;
                    
                    if retx_needed = '0' then
                        next_state <= COMPLETE;
                    end if;
                
                when RETRANSMIT_PACKET =>
                    -- This would load the stored packet from memory
                    -- For simplicity, just generating a new packet with PING frame
                    next_state <= CREATE_HEADER;
                
                when OUTPUT_DATA =>
                    -- Output data from the output buffer
                    if out_buf_rdptr /= out_buf_wrptr and data_out_ready = '1' then
                        -- Prepare 64-bit output word
                        for i in 0 to 7 loop
                            if out_buf_rdptr /= out_buf_wrptr then
                                data_out((i*8+7) downto (i*8)) <= output_buffer(out_buf_rdptr);
                                out_buf_rdptr <= (out_buf_rdptr + 1) mod BUFFER_SIZE;
                            else
                                data_out((i*8+7) downto (i*8)) <= (others => '0');
                            end if;
                        end loop;
                        
                        data_out_valid <= '1';
                        if out_buf_rdptr = out_buf_wrptr then
                            data_out_last <= '1';
                            next_state <= COMPLETE;
                        else
                            data_out_last <= '0';
                        end if;
                    else
                        data_out_valid <= '0';
                        if out_buf_rdptr = out_buf_wrptr then
                            next_state <= COMPLETE;
                        end if;
                    end if;
                
                when COMPLETE =>
                    done <= '1';
                    data_out_valid <= '0';
                    
                    -- Report validation errors by signaling the appropriate output pins
                    -- In a real implementation, you would have dedicated error reporting signals
                    -- For now, we just track the error state internally
                    
                    -- If there was a validation error, reset buffers and prepare for new data
                    if header_valid = '0' then
                        -- Reset buffer pointers to clear bad data
                        in_buf_rdptr <= 0;
                        in_buf_wrptr <= 0;
                        
                        -- In a real implementation, you would log the specific error
                        -- based on the validation_error signal here
                    end if;
                    
                    if start = '0' then
                        next_state <= IDLE;
                    end if;
                
                when others =>
                    next_state <= IDLE;
            end case;
        end if;
    end process;
    
    -- Next state logic
    process(state, start, operation, data_in_valid, data_in_last, data_out_ready, 
            in_buf_rdptr, in_buf_wrptr, out_buf_rdptr, out_buf_wrptr, retx_needed, 
            header_valid, validation_error)
    begin
        next_state <= state;
        
        case state is
            when IDLE =>
                if start = '1' then
                    case operation is
                        when "00" => next_state <= LOAD_DATA;
                        when "01" => next_state <= PROCESS_ACK;
                        when "10" => next_state <= CHECK_RETRANSMIT;
                        when others => next_state <= IDLE;
                    end case;
                end if;
                
            -- Header validation determines if packet continues or is rejected
            when VALIDATE_HEADER =>
                if header_valid = '1' then
                    next_state <= CREATE_HEADER;
                else
                    -- If header validation fails, skip to complete and report the error
                    next_state <= COMPLETE;
                end if;
            
            -- Process Payload can transition to the PROCESS_STREAM_FRAME state
            when PROCESS_PAYLOAD =>
                -- These transitions are handled in the main process
                null;
                
            -- Process Stream Frame always completes after processing
            when PROCESS_STREAM_FRAME =>
                next_state <= COMPLETE;
            
            -- Other state transitions are handled in the main process
            
            when COMPLETE =>
                if start = '0' then
                    next_state <= IDLE;
                end if;
            
            when others =>
                -- Keep current state
        end case;
    end process;

end Behavioral;
