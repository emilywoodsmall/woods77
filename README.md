<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Input Length Checker</title>
</head>
<body>
    <h2>Type Something</h2>
    
    <input type="text" id="userInput" placeholder="Enter text here">
    <button onclick="showLength()">Check Length</button>
    
    <p id="result"></p>

    <script>
        function showLength() {
            const text = document.getElementById("userInput").value;
            const length = text.length;
            document.getElementById("result").textContent = "Length: " + length;
        }
    </script>
</body>
</html>