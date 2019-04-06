import os
import sys

# Add fbcode_builder directory to the path
sys.path.append(os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "fbcode_builder"
))
