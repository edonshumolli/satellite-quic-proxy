#!/usr/bin/env python3
"""
Satellite Network Simulator

This script simulates satellite network conditions for testing the QUIC proxy.
It creates a network environment with high latency, packet loss, and variable
bandwidth characteristic of satellite communication links.
"""

import os
import sys
import time
import json
import signal
import socket
import argparse
import logging
import subprocess
import threading
from datetime import datetime

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger('satellite_sim')

class SatelliteNetworkSimulator:
    """
    Simulates satellite network conditions using Linux traffic control (tc)
    """
    
    def __init__(self, config_file):
        """
        Initialize the simulator with configuration
        
        Args:
            config_file: Path to the JSON configuration file
        """
        self.config_file = config_file
        self.load_config()
        self.running = False
        self.tc_applied = False
        self.interface = None
        
        # Register signal handlers
        signal.signal(signal.SIGINT, self.handle_signal)
        signal.signal(signal.SIGTERM, self.handle_signal)
    
    def load_config(self):
        """Load configuration from JSON file"""
        try:
            with open(self.config_file, 'r') as f:
                self.config = json.load(f)
                logger.info(f"Loaded configuration from {self.config_file}")
        except Exception as e:
            logger.error(f"Failed to load configuration: {e}")
            sys.exit(1)
    
    def detect_interface(self):
        """Detect the network interface to apply traffic control to"""
        # Default to the interface specified in the config
        self.interface = self.config.get('interface', None)
        
        if not self.interface:
            # Try to auto-detect the interface
            try:
                # Create a socket and connect to a public IP to determine outgoing interface
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                    s.connect(("8.8.8.8", 80))
                    ip = s.getsockname()[0]
                
                # Find which interface has this IP
                output = subprocess.check_output(["ip", "-o", "-4", "addr", "show"]).decode('utf-8')
                for line in output.splitlines():
                    parts = line.strip().split()
                    if ip in parts:
                        self.interface = parts[1]
                        break
            except Exception as e:
                logger.error(f"Failed to auto-detect network interface: {e}")
                sys.exit(1)
        
        if not self.interface:
            logger.error("Could not determine network interface to use")
            sys.exit(1)
        
        logger.info(f"Using network interface: {self.interface}")
    
    def apply_tc_rules(self):
        """Apply traffic control rules to simulate satellite conditions"""
        if self.tc_applied:
            logger.warning("Traffic control rules already applied. Removing and reapplying...")
            self.remove_tc_rules()
        
        try:
            # Get satellite link parameters
            latency = self.config['link']['latency_ms']
            jitter = self.config['link']['jitter_ms']
            packet_loss = self.config['link']['packet_loss_percent']
            bandwidth = self.config['link']['bandwidth_kbps']
            
            # Clear any existing rules
            subprocess.run(["tc", "qdisc", "del", "dev", self.interface, "root"], 
                          stderr=subprocess.DEVNULL)
            
            # Add qdisc with satellite link characteristics
            cmd = [
                "tc", "qdisc", "add", "dev", self.interface, "root", "netem",
                "delay", f"{latency}ms", f"{jitter}ms", "distribution", "normal",
                "loss", f"{packet_loss}%",
                "rate", f"{bandwidth}kbit"
            ]
            
            logger.info(f"Applying traffic control: {' '.join(cmd)}")
            subprocess.run(cmd, check=True)
            
            self.tc_applied = True
            logger.info(f"Applied satellite network conditions: "
                      f"Latency={latency}ms, Jitter={jitter}ms, "
                      f"Loss={packet_loss}%, Bandwidth={bandwidth}kbps")
            
        except subprocess.CalledProcessError as e:
            logger.error(f"Failed to apply traffic control rules: {e}")
            sys.exit(1)
    
    def remove_tc_rules(self):
        """Remove traffic control rules"""
        if not self.tc_applied:
            return
        
        try:
            # Clear the rules
            subprocess.run(["tc", "qdisc", "del", "dev", self.interface, "root"], check=True)
            logger.info("Removed traffic control rules")
            self.tc_applied = False
        except subprocess.CalledProcessError as e:
            logger.error(f"Failed to remove traffic control rules: {e}")
    
    def status_monitoring_thread(self):
        """Background thread to periodically update status and check link conditions"""
        while self.running:
            try:
                # Get updated network statistics
                output = subprocess.check_output(["tc", "-s", "qdisc", "show", "dev", self.interface])
                logger.debug(f"Current traffic control statistics:\n{output.decode('utf-8')}")
                
                # Verify link conditions still match configured values
                self.verify_link_conditions()
                
                # Sleep for status update interval
                time.sleep(self.config.get('status_interval_sec', 30))
            except Exception as e:
                logger.error(f"Error in status monitoring: {e}")
                time.sleep(5)  # Sleep briefly on error
    
    def verify_link_conditions(self):
        """Verify that the current link conditions match the configured values"""
        try:
            # This is a simplified check - a real implementation would verify all parameters
            output = subprocess.check_output(["tc", "qdisc", "show", "dev", self.interface])
            
            # Just check if any TC rules exist
            if b"netem" not in output:
                logger.warning("Traffic control rules appear to be missing. Reapplying...")
                self.apply_tc_rules()
        except Exception as e:
            logger.error(f"Error verifying link conditions: {e}")
    
    def start(self):
        """Start the satellite network simulator"""
        logger.info("Starting satellite network simulator")
        self.running = True
        
        # Detect the network interface to use
        self.detect_interface()
        
        # Apply traffic control rules
        self.apply_tc_rules()
        
        # Start monitoring thread
        self.monitor_thread = threading.Thread(target=self.status_monitoring_thread)
        self.monitor_thread.daemon = True
        self.monitor_thread.start()
        
        logger.info("Satellite network simulator started successfully")
        
        # If dynamic conditions are enabled, start that thread
        if self.config.get('enable_dynamic_conditions', False):
            self.start_dynamic_conditions()
    
    def start_dynamic_conditions(self):
        """Start thread to simulate dynamic changing conditions"""
        self.dynamic_thread = threading.Thread(target=self.dynamic_conditions_thread)
        self.dynamic_thread.daemon = True
        self.dynamic_thread.start()
        logger.info("Dynamic condition simulation enabled")
    
    def dynamic_conditions_thread(self):
        """Thread to periodically change network conditions"""
        scenarios = self.config.get('dynamic_scenarios', [])
        if not scenarios:
            logger.warning("Dynamic conditions enabled but no scenarios defined")
            return
        
        while self.running:
            for scenario in scenarios:
                if not self.running:
                    break
                
                # Apply this scenario
                logger.info(f"Applying dynamic scenario: {scenario['name']}")
                
                # Update config with this scenario's settings
                self.config['link']['latency_ms'] = scenario['latency_ms']
                self.config['link']['jitter_ms'] = scenario['jitter_ms']
                self.config['link']['packet_loss_percent'] = scenario['packet_loss_percent']
                self.config['link']['bandwidth_kbps'] = scenario['bandwidth_kbps']
                
                # Apply the new settings
                self.apply_tc_rules()
                
                # Wait for the scenario duration
                time.sleep(scenario['duration_sec'])
    
    def stop(self):
        """Stop the satellite network simulator"""
        logger.info("Stopping satellite network simulator")
        self.running = False
        
        # Remove traffic control rules
        self.remove_tc_rules()
        
        # Wait for monitoring thread to finish
        if hasattr(self, 'monitor_thread') and self.monitor_thread.is_alive():
            self.monitor_thread.join(timeout=2)
        
        # Wait for dynamic conditions thread to finish
        if hasattr(self, 'dynamic_thread') and self.dynamic_thread.is_alive():
            self.dynamic_thread.join(timeout=2)
        
        logger.info("Satellite network simulator stopped")
    
    def handle_signal(self, signum, frame):
        """Handle termination signals"""
        logger.info(f"Received signal {signum}, shutting down...")
        self.stop()
        sys.exit(0)

def main():
    parser = argparse.ArgumentParser(description='Satellite Network Simulator')
    parser.add_argument('-c', '--config', default='config.json',
                       help='Path to configuration file (default: config.json)')
    parser.add_argument('-v', '--verbose', action='store_true',
                       help='Enable verbose logging')
    args = parser.parse_args()
    
    # Set log level
    if args.verbose:
        logger.setLevel(logging.DEBUG)
    
    # Check if running as root (needed for tc commands)
    if os.geteuid() != 0:
        logger.error("This script requires root privileges to modify network settings.")
        sys.exit(1)
    
    try:
        # Create and start the simulator
        simulator = SatelliteNetworkSimulator(args.config)
        simulator.start()
        
        # Keep the script running
        logger.info("Simulator running. Press Ctrl+C to stop.")
        while simulator.running:
            time.sleep(1)
    except KeyboardInterrupt:
        logger.info("Interrupted by user")
    except Exception as e:
        logger.error(f"Error in main loop: {e}")
    finally:
        if 'simulator' in locals():
            simulator.stop()

if __name__ == "__main__":
    main()
