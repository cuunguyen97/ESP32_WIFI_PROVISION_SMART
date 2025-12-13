<script>
        function send(cmd) {
            fetch(cmd)
                .then(() => updateState())
                .catch(() => alert('Lỗi kết nối!'));
        }

        function updateState() {
            fetch('/state')
                .then(r => r.text())
                .then(status => {
                    const el = document.getElementById('state');
                    el.innerText = status;
                    el.className = status.includes('BẬT') ? 'on' : 'off';
                })
                .catch(() => {
                    document.getElementById('state').innerText = 'Mất kết nối';
                });
        }

        // Cập nhật mỗi 2 giây + tải ngay lần đầu
        setInterval(updateState, 2000);
        updateState();
    </script>